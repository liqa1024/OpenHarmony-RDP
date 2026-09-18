# 22) HmRdp: one arena for the tile decode's persistent buffers.
#
#     动机（doc_agent/cpu-accel-plan.md §1）：并行下相位占比显示**访存型相位膨胀、算力型不膨胀**——
#     `color` 每 tile 放大 ~9.5×、`dequant` ~8.9×、`idwt` ~6.7×，而纯位解码的 `rlgr` 只有 ~3.7×。
#     原因是每个 tile 的三份持久缓冲（`sign` 24KB、`current` 24KB、`data` 16KB）都是**各自一次
#     malloc**，14 个 worker 同时打到 14 片互不相邻的页 ⇒ TLB / DRAM 行冲突。
#
#     这一步把三份缓冲改成**每个 surface 一整块 arena**，布局是**按 tile 连续**（tile i 的
#     sign/current/data 在同一段 64KB 里，且每段起点 64 字节对齐），于是"一个 worker 解一个 tile"
#     只碰一段连续内存。缓冲区内部的**分量偏移完全不动**（sign/current 仍是每分量
#     `(8192+32)` 字节、起点 +16），只是换了底层存储 ⇒ 像素逐位不变。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progArenaH = "$Source\libfreerdp\codec\progressive.h"
$progArenaC = "$Source\libfreerdp\codec\progressive.c"

# (a) the surface owns the arena.
Patch-Regex $progArenaH `
  '\tBOOL hmrdpDirtyAny;\n\} PROGRESSIVE_SURFACE_CONTEXT;' (@'
	BOOL hmrdpDirtyAny;
	/* HmRdp: one contiguous arena holding every tile's persistent buffers
	 * (sign / current / data), laid out tile-major and cache-line aligned, so the
	 * worker decoding one tile touches one contiguous block instead of three
	 * scattered heap allocations. Owned by the surface. */
	BYTE* hmrdpTileArena;
	size_t hmrdpTileArenaBytes;
} PROGRESSIVE_SURFACE_CONTEXT;
'@) 'hmrdpTileArena'

# (b) the layout constants and the resize helper.
Patch-Regex $progArenaC `
  'static void progressive_tile_free\(RFX_PROGRESSIVE_TILE\* WINPR_RESTRICT tile\)' (@'
/*
 * HmRdp: the tile arena. Every tile gets one contiguous block holding its three
 * persistent buffers, each region starting on a cache line:
 *
 *     [ sign 24672 -> 24704 ][ current 24672 -> 24704 ][ data 16384 ]
 *
 * The component offsets *inside* sign/current are the upstream ones; only the
 * backing storage changes, so the decoded pixels do not.
 */
#define HMRDP_TILE_BYTES (((8192ULL + 32ULL) * 3ULL))
#define HMRDP_TILE_ALIGNED ((HMRDP_TILE_BYTES + 63ULL) & ~63ULL)
#define HMRDP_TILE_DATA_BYTES (64ULL * 4ULL * 64ULL)
#define HMRDP_TILE_ARENA_STRIDE (HMRDP_TILE_ALIGNED * 2ULL + HMRDP_TILE_DATA_BYTES)

/* Dev A/B toggle: 1 = the three buffers come from the surface's arena (tile-major
 * and cache-line aligned), 0 = every tile mallocs its own, which is the layout
 * before this step. Both are pixel-identical; only the addresses differ, so this
 * is how the arena's effect is measured at a fixed width. */
#define HMRDP_TILE_ARENA 1

static INLINE void hmrdp_tile_bind_arena(PROGRESSIVE_SURFACE_CONTEXT* WINPR_RESTRICT surface,
                                         RFX_PROGRESSIVE_TILE* WINPR_RESTRICT tile, size_t index)
{
#if HMRDP_TILE_ARENA
	BYTE* base = surface->hmrdpTileArena + (index * (size_t)HMRDP_TILE_ARENA_STRIDE);

	tile->sign = base;
	tile->current = base + (size_t)HMRDP_TILE_ALIGNED;
	tile->data = base + ((size_t)HMRDP_TILE_ALIGNED * 2ULL);
#else
	WINPR_UNUSED(surface);
	WINPR_UNUSED(index);

	tile->sign = (BYTE*)winpr_aligned_malloc((size_t)HMRDP_TILE_BYTES, 16);
	tile->current = (BYTE*)winpr_aligned_malloc((size_t)HMRDP_TILE_BYTES, 16);
	tile->data = (BYTE*)winpr_aligned_malloc((size_t)HMRDP_TILE_DATA_BYTES, 16);
#endif
}

/* One arena for `tiles` tiles. The layout does not depend on the tile count, so
 * growing the cache is a single copy of the old blocks - the persistent
 * predictor state has to survive that. */
static INLINE BOOL hmrdp_tile_arena_resize(PROGRESSIVE_SURFACE_CONTEXT* WINPR_RESTRICT surface,
                                           size_t tiles)
{
	const size_t stride = (size_t)HMRDP_TILE_ARENA_STRIDE;
	BYTE* arena = NULL;

	WINPR_ASSERT(surface);

	arena = (BYTE*)winpr_aligned_malloc(tiles * stride, 64);
	if (!arena)
		return FALSE;

	if (surface->hmrdpTileArena)
	{
		const size_t oldTiles = surface->hmrdpTileArenaBytes / stride;
		const size_t copy = (tiles < oldTiles) ? tiles : oldTiles;

		if (copy > 0)
			CopyMemory(arena, surface->hmrdpTileArena, copy * stride);
		winpr_aligned_free(surface->hmrdpTileArena);
	}
	surface->hmrdpTileArena = arena;
	surface->hmrdpTileArenaBytes = tiles * stride;
	return TRUE;
}

static void progressive_tile_free(RFX_PROGRESSIVE_TILE* WINPR_RESTRICT tile)
'@) 'HMRDP_TILE_ARENA_STRIDE'

# (c) the tile struct no longer owns its buffers.
Patch-Regex $progArenaC `
  'static void progressive_tile_free\(RFX_PROGRESSIVE_TILE\* WINPR_RESTRICT tile\)\n\{\n\tif \(tile\)\n\t\{\n\t\twinpr_aligned_free\(tile->sign\);\n\t\twinpr_aligned_free\(tile->current\);\n\t\twinpr_aligned_free\(tile->data\);\n\t\twinpr_aligned_free\(tile\);\n\t\}\n\}' (@'
static void progressive_tile_free(RFX_PROGRESSIVE_TILE* WINPR_RESTRICT tile)
{
	if (tile)
	{
#if HMRDP_TILE_ARENA
		/* HmRdp: sign/current/data point into the surface's tile arena, which the
		 * surface owns - only the tile struct itself is freed here. */
		winpr_aligned_free(tile);
#else
		/* HmRdp dev A/B: the pre-arena layout owns its three buffers. */
		winpr_aligned_free(tile->sign);
		winpr_aligned_free(tile->current);
		winpr_aligned_free(tile->data);
		winpr_aligned_free(tile);
#endif
	}
}
'@) 'surface owns - only the tile struct'

# (d) the tile takes its buffers from the arena.
Patch-Regex $progArenaC `
  'static INLINE RFX_PROGRESSIVE_TILE\* progressive_tile_new\(void\)\n\{\n.*?\nfail:\n\tprogressive_tile_free\(tile\);\n\treturn NULL;\n\}' (@'
static INLINE RFX_PROGRESSIVE_TILE*
progressive_tile_new(PROGRESSIVE_SURFACE_CONTEXT* WINPR_RESTRICT surface, size_t index)
{
	RFX_PROGRESSIVE_TILE* tile = winpr_aligned_calloc(1, sizeof(RFX_PROGRESSIVE_TILE), 32);
	if (!tile)
		return NULL;

	tile->width = 64;
	tile->height = 64;
	tile->stride = 4 * tile->width;

	/* HmRdp: the three persistent buffers are carved out of the surface's tile
	 * arena, one cache-line aligned block per tile (see hmrdp_tile_bind_arena). */
	hmrdp_tile_bind_arena(surface, tile, index);

	/* HmRdp: the per-(tile,component) predictor state must start defined. The
	 * encoder assumes the client's `current`/`sign` start at zero (a difference /
	 * upgrade pass reads them before writing), so leaving the arena's contents
	 * here would make the decoder's output depend on unrelated writes. */
	memset(tile->sign, 0, (size_t)HMRDP_TILE_BYTES);
	memset(tile->current, 0, (size_t)HMRDP_TILE_BYTES);
	memset(tile->data, 0xFF, (size_t)HMRDP_TILE_DATA_BYTES);

	return tile;
}
'@) 'see hmrdp_tile_bind_arena'

# (e) the cache allocates the arena and keeps the existing tiles pointing into it.
Patch-Regex $progArenaC `
  '\tsurface->tilesSize = surface->gridSize;\n\tsurface->tiles = \(RFX_PROGRESSIVE_TILE\*\*\)tmp;\n\n\tfor \(size_t x = oldIndex; x < surface->tilesSize; x\+\+\)\n\t\{\n\t\tsurface->tiles\[x\] = progressive_tile_new\(\);\n\t\tif \(!surface->tiles\[x\]\)\n\t\t\treturn FALSE;\n\t\}' (@'
	surface->tilesSize = surface->gridSize;
	surface->tiles = (RFX_PROGRESSIVE_TILE**)tmp;

#if HMRDP_TILE_ARENA
	/* HmRdp: one arena for every tile's persistent buffers, sized to the tile
	 * cache - it only changes when the cache grows. */
	if (!hmrdp_tile_arena_resize(surface, surface->tilesSize))
		return FALSE;

	/* The arena may have moved: re-point the tiles that already exist (their
	 * predictor state was copied over with it). */
	for (size_t x = 0; x < oldIndex; x++)
	{
		RFX_PROGRESSIVE_TILE* tile = surface->tiles[x];

		if (tile)
			hmrdp_tile_bind_arena(surface, tile, x);
	}
#endif

	for (size_t x = oldIndex; x < surface->tilesSize; x++)
	{
		surface->tiles[x] = progressive_tile_new(surface, x);
		if (!surface->tiles[x])
			return FALSE;
	}
'@) 'one arena for every tile''s persistent buffers'

# (f) release the arena with the surface.
Patch-Regex $progArenaC `
  '\twinpr_aligned_free\(\(void\*\)surface->tiles\);\n\twinpr_aligned_free\(surface->updatedTileIndices\);' (@'
	winpr_aligned_free((void*)surface->tiles);
	winpr_aligned_free(surface->hmrdpTileArena);
	winpr_aligned_free(surface->updatedTileIndices);
'@) 'winpr_aligned_free(surface->hmrdpTileArena);'
