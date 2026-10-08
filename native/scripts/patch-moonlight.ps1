# Applies the HmRdp patches to the moonlight-common-c source tree:
#   1) strip the unconditional -Werror (CMakeLists.txt);
#   2) add the foundation-sunshine "local cursor mode" client side:
#      ConnListenerCursorUpdate + LiGetLocalCursorModeSupported /
#      LiSetLocalCursorMode and the 0x5509 control-stream message.
#
# The tree under native/third_party/ is gitignored and recreated by
# fetch-sources.ps1, which calls this script after extraction. Like
# patch-freerdp.ps1 it is idempotent: an already-patched tree reports the step
# as skipped instead of failing.
#
#   native/scripts/patch-moonlight.ps1 [-Source <tree>]
param(
	[string]$Source = ""
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
if ($Source.Length -eq 0) {
	$Source = Join-Path $Native "third_party\moonlight-common-c"
}
$CMake = Join-Path $Source "CMakeLists.txt"

if (-not (Test-Path -LiteralPath $CMake)) {
	throw "moonlight-common-c CMakeLists not found: $CMake"
}

# Read raw and write back with the file's own line endings preserved: a plain
# string replace does not touch them, so LF (tarball) and CRLF (checkout) both work.
$raw = [System.IO.File]::ReadAllText($CMake)

# 1) Drop the unconditional -Werror the upstream CMakeLists adds on non-MSVC /
#    non-MinGW platforms. The OHOS clang does not carry the warning set upstream
#    CI does, and -Werror is added *after* CMAKE_C_FLAGS so it cannot be turned
#    off from the command line. Warnings stay visible; they just do not fail the
#    build.
if ($raw -notmatch [regex]::Escape(" -Werror)")) {
	Write-Host "moonlight: -Werror already stripped (skipped)"
}
else {
	$patched = $raw -replace [regex]::Escape(" -Werror)"), ")"
	if ($patched -eq $raw) {
		throw "moonlight: -Werror present but not matched"
	}
	[System.IO.File]::WriteAllText($CMake, $patched)
	Write-Host "moonlight: stripped -Werror from CMakeLists.txt"
}

# 2) Local cursor mode. foundation-sunshine advertises a cursor_shape capability
#    bit in the RTSP DESCRIBE featureFlags and, once the client asks for local
#    cursor mode over the control stream, stops compositing the pointer into the
#    video and forwards the pointer (shape bitmap + visibility) on an extension
#    control message instead. The client installs the shape as the OS cursor, so
#    there is exactly one, locally-rendered cursor. All additions are wrapped in
#    HMRDP markers so the step is idempotent.
function Set-PatchedFile {
	param([string]$Path, [string]$Eol, [string]$PatchedContent)

	if ($PatchedContent -eq (Get-Content -LiteralPath $Path -Raw)) {
		return $false
	}
	[System.IO.File]::WriteAllText($Path, $PatchedContent)
	return $true
}

$MlSrc = Join-Path $Source "src"
$Limelight = Join-Path $MlSrc "Limelight.h"
$ControlStream = Join-Path $MlSrc "ControlStream.c"
if (-not (Test-Path -LiteralPath $Limelight) -or -not (Test-Path -LiteralPath $ControlStream)) {
	throw "moonlight-common-c src missing Limelight.h / ControlStream.c under $MlSrc"
}

# --- Limelight.h -------------------------------------------------------------
$hRaw = [System.IO.File]::ReadAllText($Limelight)
if ($hRaw.Contains("HMRDP_CURSOR")) {
	Write-Host "moonlight: Limelight.h cursor patch already applied (skipped)"
}
else {
	$heol = if ($hRaw.Contains("`r`n")) { "`r`n" } else { "`n" }

	$cbType = @(
		"typedef void(*ConnListenerCursorUpdate)(bool visible, bool hasShape, uint32_t shapeId,",
		"                                        uint16_t width, uint16_t height,",
		"                                        int16_t hotspotX, int16_t hotspotY,",
		"                                        const uint8_t* bgra, uint32_t size); // HMRDP_CURSOR"
	) -join $heol

	$anchorStruct = "typedef struct _CONNECTION_LISTENER_CALLBACKS {"
	if (-not $hRaw.Contains($anchorStruct)) { throw "moonlight: CONNECTION_LISTENER_CALLBACKS anchor missing" }
	$hRaw = $hRaw.Replace($anchorStruct, $cbType + $heol + $heol + $anchorStruct)

	$anchorField = "    ConnListenerSetAdaptiveTriggers setAdaptiveTriggers;"
	if (-not $hRaw.Contains($anchorField)) { throw "moonlight: adaptive triggers field anchor missing" }
	$hRaw = $hRaw.Replace($anchorField, $anchorField + $heol + "    ConnListenerCursorUpdate cursorUpdate; // HMRDP_CURSOR")

	$anchorInit = "void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS clCallbacks);"
	if (-not $hRaw.Contains($anchorInit)) { throw "moonlight: callback init decl anchor missing" }
	$proto = @(
		"void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS clCallbacks);",
		"",
		"// HMRDP_CURSOR: foundation-sunshine local cursor mode.",
		"// True when the connected host advertised the cursor_shape capability.",
		"bool LiGetLocalCursorModeSupported(void);",
		"// Ask the host to stop compositing the pointer into the video and forward it",
		"// through ConnListenerCursorUpdate instead. Call after LiStartConnection().",
		"void LiSetLocalCursorMode(bool enabled);"
	) -join $heol
	$hRaw = $hRaw.Replace($anchorInit, $proto)

	[System.IO.File]::WriteAllText($Limelight, $hRaw)
	Write-Host "moonlight: added local cursor callback/API to Limelight.h"
}

# --- ControlStream.c ---------------------------------------------------------
$cRaw = [System.IO.File]::ReadAllText($ControlStream)
if ($cRaw.Contains("HMRDP_CURSOR")) {
	Write-Host "moonlight: ControlStream.c cursor patch already applied (skipped)"
}
else {
	$ceol = if ($cRaw.Contains("`r`n")) { "`r`n" } else { "`n" }

	# Receive-side helper: parse the 28-byte header + BGRA chunks, reassemble a
	# shape, and deliver it (or a visibility-only update) to the listener.
	$helper = @(
		"// HMRDP_CURSOR: foundation-sunshine local cursor mode (control message 0x5509).",
		"#define ML_CURSOR_CTRL_TYPE 0x5509",
		"#define ML_SS_FF_CURSOR_SHAPE 0x40u",
		"#define ML_CURSOR_FLAG_SHAPE 0x01",
		"#define ML_CURSOR_FLAG_VISIBLE 0x02",
		"#define ML_CURSOR_HEADER_SIZE 28",
		"",
		"static uint8_t* mlCursorBuffer;",
		"static uint32_t mlCursorCapacity;",
		"static uint32_t mlCursorTotal;",
		"static uint32_t mlCursorReceived;",
		"static uint32_t mlCursorShapeId;",
		"static uint16_t mlCursorWidth;",
		"static uint16_t mlCursorHeight;",
		"static int16_t mlCursorHotspotX;",
		"static int16_t mlCursorHotspotY;",
		"",
		"static void handleLocalCursorUpdate(PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr, int packetLength) {",
		"    const uint8_t* payload = (const uint8_t*)ctlHdr + sizeof(*ctlHdr);",
		"    int payloadLength = packetLength - (int)sizeof(*ctlHdr);",
		"    const uint8_t* chunkData = payload + ML_CURSOR_HEADER_SIZE;",
		"    int chunkAvail = payloadLength - ML_CURSOR_HEADER_SIZE;",
		"    bool shape;",
		"    bool visible;",
		"    uint32_t shapeId;",
		"    uint32_t total;",
		"    uint32_t offset;",
		"    uint16_t chunk;",
		"    if (payloadLength < ML_CURSOR_HEADER_SIZE || ListenerCallbacks.cursorUpdate == NULL) {",
		"        return;",
		"    }",
		"    shape = (payload[1] & ML_CURSOR_FLAG_SHAPE) != 0;",
		"    visible = (payload[1] & ML_CURSOR_FLAG_VISIBLE) != 0;",
		"    shapeId = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |",
		"              ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);",
		"    total = (uint32_t)payload[16] | ((uint32_t)payload[17] << 8) |",
		"            ((uint32_t)payload[18] << 16) | ((uint32_t)payload[19] << 24);",
		"    offset = (uint32_t)payload[20] | ((uint32_t)payload[21] << 8) |",
		"             ((uint32_t)payload[22] << 16) | ((uint32_t)payload[23] << 24);",
		"    chunk = (uint16_t)payload[24] | ((uint16_t)payload[25] << 8);",
		"    if (!shape) {",
		"        ListenerCallbacks.cursorUpdate(visible, false, shapeId, 0, 0, 0, 0, NULL, 0);",
		"        return;",
		"    }",
		"    if (offset == 0) {",
		"        mlCursorReceived = 0;",
		"        mlCursorShapeId = shapeId;",
		"        mlCursorWidth = (uint16_t)payload[8] | ((uint16_t)payload[9] << 8);",
		"        mlCursorHeight = (uint16_t)payload[10] | ((uint16_t)payload[11] << 8);",
		"        mlCursorHotspotX = (int16_t)((uint16_t)payload[12] | ((uint16_t)payload[13] << 8));",
		"        mlCursorHotspotY = (int16_t)((uint16_t)payload[14] | ((uint16_t)payload[15] << 8));",
		"        mlCursorTotal = total;",
		"        if (total > mlCursorCapacity) {",
		"            uint8_t* grown = (uint8_t*)realloc(mlCursorBuffer, total);",
		"            if (grown == NULL) {",
		"                mlCursorCapacity = 0;",
		"                mlCursorTotal = 0;",
		"                return;",
		"            }",
		"            mlCursorBuffer = grown;",
		"            mlCursorCapacity = total;",
		"        }",
		"    }",
		"    if (mlCursorBuffer == NULL || offset != mlCursorReceived ||",
		"        (uint64_t)offset + (uint64_t)chunk > (uint64_t)mlCursorTotal) {",
		"        return;",
		"    }",
		"    if (chunk > (uint16_t)chunkAvail) {",
		"        chunk = (uint16_t)chunkAvail;",
		"    }",
		"    if (chunk != 0) {",
		"        memcpy(mlCursorBuffer + offset, chunkData, chunk);",
		"        mlCursorReceived = offset + chunk;",
		"    }",
		"    if (mlCursorTotal != 0 && mlCursorReceived >= mlCursorTotal) {",
		"        ListenerCallbacks.cursorUpdate(true, true, mlCursorShapeId, mlCursorWidth, mlCursorHeight,",
		"                                       mlCursorHotspotX, mlCursorHotspotY, mlCursorBuffer,",
		"                                       mlCursorTotal);",
		"    }",
		"}",
		""
	) -join $ceol

	$anchorReceive = "static void controlReceiveThreadFunc(void* context) {"
	if (-not $cRaw.Contains($anchorReceive)) { throw "moonlight: controlReceiveThreadFunc anchor missing" }
	$cRaw = $cRaw.Replace($anchorReceive, $helper + $ceol + $anchorReceive)

	# Dispatch the extension message on the receive thread.
	$anchorDispatch = "            else if (ctlHdr->type == packetTypes[IDX_TERMINATION]) {"
	if (-not $cRaw.Contains($anchorDispatch)) { throw "moonlight: termination dispatch anchor missing" }
	$dispatch = @(
		"            else if (ctlHdr->type == ML_CURSOR_CTRL_TYPE) { // HMRDP_CURSOR",
		"                handleLocalCursorUpdate(ctlHdr, packetLength);",
		"            }",
		$anchorDispatch
	) -join $ceol
	$cRaw = $cRaw.Replace($anchorDispatch, $dispatch)

	# Public API at the end of the file (after sendMessageAndForget is defined).
	$api = @(
		"",
		"// HMRDP_CURSOR: foundation-sunshine local cursor mode.",
		"bool LiGetLocalCursorModeSupported(void) {",
		"    return IS_SUNSHINE() && (SunshineFeatureFlags & ML_SS_FF_CURSOR_SHAPE) != 0;",
		"}",
		"",
		"void LiSetLocalCursorMode(bool enabled) {",
		"    unsigned char payload[4];",
		"    if (!LiGetLocalCursorModeSupported() || !encryptedControlStream) {",
		"        return;",
		"    }",
		"    payload[0] = 1; // cursor protocol version",
		"    payload[1] = enabled ? 1 : 0; // 1 = local cursor mode on",
		"    payload[2] = 0;",
		"    payload[3] = 0;",
		"    sendMessageAndForget(ML_CURSOR_CTRL_TYPE, (int)sizeof(payload), payload,",
		"                         CTRL_CHANNEL_GENERIC, 0, false);",
		"}"
	) -join $ceol
	$cRaw = $cRaw.TrimEnd() + $ceol + $api + $ceol

	[System.IO.File]::WriteAllText($ControlStream, $cRaw)
	Write-Host "moonlight: added local cursor message handling to ControlStream.c"
}

Write-Host "=== moonlight-common-c patches applied ==="
