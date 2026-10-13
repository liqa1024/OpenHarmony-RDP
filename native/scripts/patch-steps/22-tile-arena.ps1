# 22) One contiguous arena per surface for the tile decode's persistent buffers (sign/current/data),
#     tile-major and cache-line aligned, so a worker touches one block; component offsets unchanged.
$progArenaH = "$Source\libfreerdp\codec\progressive.h"
$progArenaC = "$Source\libfreerdp\codec\progressive.c"

Patch-Regex $progArenaH `
  '\tBOOL hmrdpDirtyAny;\n\} PROGRESSIVE_SURFACE_CONTEXT;' (@'
	BOOL hmrdpDirtyAny;
	/* HmRdp: one tile-major, cache-line-aligned arena for every tile's sign/current/data; owned by the surface. */
	BYTE* hmrdpTileArena;
	size_t hmrdpTileArenaBytes;
} PROGRESSIVE_SURFACE_CONTEXT;
'@) 'hmrdpTileArena'

Patch-Regex $progArenaC `
  'static void progressive_tile_free\(RFX_PROGRESSIVE_TILE\* WINPR_RESTRICT tile\)' (@'
/* HmRdp: the tile arena: one cache-line-aligned block per tile holding sign/current/data; component offsets inside are the upstream ones. */
#define HMRDP_TILE_BYTES (((8192ULL + 32ULL) * 3ULL))
#define HMRDP_TILE_ALIGNED ((HMRDP_TILE_BYTES + 63ULL) & ~63ULL)
#define HMRDP_TILE_DATA_BYTES (64ULL * 4ULL * 64ULL)
#define HMRDP_TILE_ARENA_STRIDE (HMRDP_TILE_ALIGNED * 2ULL + HMRDP_TILE_DATA_BYTES)

/* Dev A/B toggle: 1 = surface arena, 0 = per-tile malloc (pixel-identical, only addresses differ). */
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

/* One arena for `tiles` tiles; growing copies the old blocks so the persistent predictor state survives. */
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

Patch-Regex $progArenaC `
  'static void progressive_tile_free\(RFX_PROGRESSIVE_TILE\* WINPR_RESTRICT tile\)\n\{\n\tif \(tile\)\n\t\{\n\t\twinpr_aligned_free\(tile->sign\);\n\t\twinpr_aligned_free\(tile->current\);\n\t\twinpr_aligned_free\(tile->data\);\n\t\twinpr_aligned_free\(tile\);\n\t\}\n\}' (@'
static void progressive_tile_free(RFX_PROGRESSIVE_TILE* WINPR_RESTRICT tile)
{
	if (tile)
	{
#if HMRDP_TILE_ARENA
		/* HmRdp: sign/current/data point into the surface's arena, which the surface owns - only the tile struct is freed here. */
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

	/* HmRdp: the three persistent buffers come from the surface's tile arena (see hmrdp_tile_bind_arena). */
	hmrdp_tile_bind_arena(surface, tile, index);

	/* HmRdp: predictor state must start defined (a difference/upgrade pass reads before writing), so zero it. */
	memset(tile->sign, 0, (size_t)HMRDP_TILE_BYTES);
	memset(tile->current, 0, (size_t)HMRDP_TILE_BYTES);
	memset(tile->data, 0xFF, (size_t)HMRDP_TILE_DATA_BYTES);

	return tile;
}
'@) 'see hmrdp_tile_bind_arena'

Patch-Regex $progArenaC `
  '\tsurface->tilesSize = surface->gridSize;\n\tsurface->tiles = \(RFX_PROGRESSIVE_TILE\*\*\)tmp;\n\n\tfor \(size_t x = oldIndex; x < surface->tilesSize; x\+\+\)\n\t\{\n\t\tsurface->tiles\[x\] = progressive_tile_new\(\);\n\t\tif \(!surface->tiles\[x\]\)\n\t\t\treturn FALSE;\n\t\}' (@'
	surface->tilesSize = surface->gridSize;
	surface->tiles = (RFX_PROGRESSIVE_TILE**)tmp;

#if HMRDP_TILE_ARENA
	/* HmRdp: one arena for every tile's persistent buffers, sized to the tile cache. */
	if (!hmrdp_tile_arena_resize(surface, surface->tilesSize))
		return FALSE;

	/* The arena may have moved: re-point existing tiles (their state was copied over). */
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

Patch-Regex $progArenaC `
  '\twinpr_aligned_free\(\(void\*\)surface->tiles\);\n\twinpr_aligned_free\(surface->updatedTileIndices\);' (@'
	winpr_aligned_free((void*)surface->tiles);
	winpr_aligned_free(surface->hmrdpTileArena);
	winpr_aligned_free(surface->updatedTileIndices);
'@) 'winpr_aligned_free(surface->hmrdpTileArena);'
