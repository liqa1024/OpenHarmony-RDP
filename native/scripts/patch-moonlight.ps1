# Applies the HmRdp patches to the moonlight-common-c source tree:
#   1) strip the unconditional -Werror (CMakeLists.txt);
#   2) add the foundation-sunshine "local cursor mode" client side:
#      ConnListenerCursorUpdate + LiGetLocalCursorModeSupported /
#      LiSetLocalCursorMode and the 0x5509 control-stream message.
#   3) add the foundation-sunshine native precision touchpad client side:
#      LiSendTouchpadEvent / LiSendTouchpadFrameEvent + the 0x55000009 /
#      0x5500000A input packets, gated on the host featureFlags bits 0x10 / 0x20.
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
		"void LiSetLocalCursorMode(bool enabled) {",
		"    unsigned char payload[4];",
		"    // Gated on a stable host identity + the encrypted v2 control stream",
		"    // only. The host re-validates at enable time (encrypted control and an",
		"    // available cursor producer, which exists once the stream is up) and",
		"    // silently ignores the request otherwise - that is the fallback.",
		"    if (!IS_SUNSHINE() || !encryptedControlStream) {",
		"        return;",
		"    }",
		"    payload[0] = 1; // cursor protocol version",
		"    payload[1] = enabled ? 1 : 0; // 1 = local cursor mode on",
		"    payload[2] = 0;",
		"    payload[3] = 0;",
		"    // The enable is a one-shot handshake for the whole connection and the",
		"    // protocol has no ACK or retransmit: an unreliable send that is lost",
		"    // silently disables the cursor channel for the session's lifetime, so",
		"    // it goes reliable (ENet retransmits until ACKed), same as the",
		"    // periodic ping upstream already sends reliably.",
		"    sendMessageAndForget(ML_CURSOR_CTRL_TYPE, (int)sizeof(payload), payload,",
		"                         CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false);",
		"}"
	) -join $ceol
	$cRaw = $cRaw.TrimEnd() + $ceol + $api + $ceol

	[System.IO.File]::WriteAllText($ControlStream, $cRaw)
	Write-Host "moonlight: added local cursor message handling to ControlStream.c"
}

# 3) Native precision touchpad (foundation-sunshine extension). Adds
#    LiSendTouchpadEvent / LiSendTouchpadFrameEvent and the 0x55000009 /
#    0x5500000A input packets, gated on the host featureFlags bits 0x10 / 0x20
#    (x-ss-general.featureFlags, already parsed into SunshineFeatureFlags).
#    Stock Sunshine and GFE never advertise those bits, so the client never sends
#    these packets to them - the LiSend* entry points return LI_ERR_UNSUPPORTED.
$InputHeader = Join-Path $MlSrc "Input.h"
$InputStream = Join-Path $MlSrc "InputStream.c"
if (-not (Test-Path -LiteralPath $InputHeader) -or -not (Test-Path -LiteralPath $InputStream)) {
	throw "moonlight-common-c src missing Input.h / InputStream.c under $MlSrc"
}

# --- Limelight.h: feature bits + API ------------------------------------------
$hRaw = [System.IO.File]::ReadAllText($Limelight)
if ($hRaw.Contains("HMRDP_TOUCHPAD")) {
	Write-Host "moonlight: Limelight.h touchpad patch already applied (skipped)"
}
else {
	$heol = if ($hRaw.Contains("`r`n")) { "`r`n" } else { "`n" }

	$anchorFF = "#define LI_FF_CONTROLLER_TOUCH_EVENTS 0x02 // LiSendControllerTouchEvent() supported"
	if (-not $hRaw.Contains($anchorFF)) { throw "moonlight: LI_FF anchor missing in Limelight.h" }
	$hRaw = $hRaw.Replace($anchorFF, $anchorFF + $heol +
		"#define LI_FF_TOUCHPAD_EVENTS         0x10 // LiSendTouchpadEvent() supported // HMRDP_TOUCHPAD" + $heol +
		"#define LI_FF_TOUCHPAD_FRAME_EVENTS   0x20 // LiSendTouchpadFrameEvent() supported // HMRDP_TOUCHPAD")

	$anchorTouch = @(
		"int LiSendTouchEvent(uint8_t eventType, uint32_t pointerId, float x, float y, float pressureOrDistance,",
		"                     float contactAreaMajor, float contactAreaMinor, uint16_t rotation);"
	) -join $heol
	if (-not $hRaw.Contains($anchorTouch)) { throw "moonlight: LiSendTouchEvent anchor missing in Limelight.h" }
	$decls = @(
		$anchorTouch,
		"",
		"// HMRDP_TOUCHPAD: foundation-sunshine native precision touchpad extension.",
		"#define LI_TOUCHPAD_BUTTON_PRIMARY 0x01",
		"int LiSendTouchpadEvent(uint8_t eventType, uint32_t pointerId, float x, float y, float pressure,",
		"                        float contactAreaMajor, float contactAreaMinor, uint16_t rotation,",
		"                        uint16_t deviceWidthMm, uint16_t deviceHeightMm, uint8_t buttonState);",
		"int LiSendTouchpadFrameEvent(uint8_t contactCount, const uint8_t* eventTypes, const uint32_t* pointerIds,",
		"                             const float* x, const float* y, const float* pressure, uint16_t rotation,",
		"                             uint16_t deviceWidthMm, uint16_t deviceHeightMm, uint8_t buttonState);"
	) -join $heol
	$hRaw = $hRaw.Replace($anchorTouch, $decls)

	[System.IO.File]::WriteAllText($Limelight, $hRaw)
	Write-Host "moonlight: added native touchpad API to Limelight.h"
}

# --- Input.h: packet layouts --------------------------------------------------
$ihRaw = [System.IO.File]::ReadAllText($InputHeader)
if ($ihRaw.Contains("HMRDP_TOUCHPAD")) {
	Write-Host "moonlight: Input.h touchpad patch already applied (skipped)"
}
else {
	$ieol = if ($ihRaw.Contains("`r`n")) { "`r`n" } else { "`n" }
	$anchorPen = "} SS_PEN_PACKET, *PSS_PEN_PACKET;"
	if (-not $ihRaw.Contains($anchorPen)) { throw "moonlight: SS_PEN_PACKET anchor missing in Input.h" }
	$structs = @(
		$anchorPen,
		"",
		"// HMRDP_TOUCHPAD: foundation-sunshine native precision touchpad (x/y over the",
		"// physical pad surface, deviceWidthMm/HeightMm = physical size).",
		"#define SS_TOUCHPAD_MAGIC 0x55000009",
		"#define SS_TOUCHPAD_BUTTON_PRIMARY 0x01",
		"typedef struct _SS_TOUCHPAD_PACKET {",
		"    NV_INPUT_HEADER header;",
		"    uint8_t eventType;",
		"    uint8_t buttonState;",
		"    uint16_t rotation;",
		"    uint32_t pointerId;",
		"    uint16_t deviceWidthMm;",
		"    uint16_t deviceHeightMm;",
		"    netfloat x;",
		"    netfloat y;",
		"    netfloat pressure;",
		"    netfloat contactAreaMajor;",
		"    netfloat contactAreaMinor;",
		"} SS_TOUCHPAD_PACKET, *PSS_TOUCHPAD_PACKET;",
		"",
		"#define SS_TOUCHPAD_FRAME_MAGIC 0x5500000A",
		"#define SS_TOUCHPAD_FRAME_MAX_CONTACTS 5",
		"typedef struct _SS_TOUCHPAD_FRAME_CONTACT {",
		"    uint8_t eventType;",
		"    uint8_t zero[1]; // Alignment/reserved",
		"    uint16_t pressure;",
		"    uint32_t pointerId;",
		"    uint16_t x;",
		"    uint16_t y;",
		"} SS_TOUCHPAD_FRAME_CONTACT, *PSS_TOUCHPAD_FRAME_CONTACT;",
		"",
		"typedef struct _SS_TOUCHPAD_FRAME_PACKET {",
		"    NV_INPUT_HEADER header;",
		"    uint8_t contactCount;",
		"    uint8_t buttonState;",
		"    uint16_t rotation;",
		"    uint16_t deviceWidthMm;",
		"    uint16_t deviceHeightMm;",
		"    SS_TOUCHPAD_FRAME_CONTACT contacts[SS_TOUCHPAD_FRAME_MAX_CONTACTS];",
		"} SS_TOUCHPAD_FRAME_PACKET, *PSS_TOUCHPAD_FRAME_PACKET;"
	) -join $ieol
	$ihRaw = $ihRaw.Replace($anchorPen, $structs)
	[System.IO.File]::WriteAllText($InputHeader, $ihRaw)
	Write-Host "moonlight: added touchpad packets to Input.h"
}

# --- InputStream.c: union, state, send ----------------------------------------
$icRaw = [System.IO.File]::ReadAllText($InputStream)
if ($icRaw.Contains("HMRDP_TOUCHPAD")) {
	Write-Host "moonlight: InputStream.c touchpad patch already applied (skipped)"
}
else {
	$ieol = if ($icRaw.Contains("`r`n")) { "`r`n" } else { "`n" }

	$anchorUnion = "        SS_TOUCH_PACKET touch;" + $ieol + "        SS_PEN_PACKET pen;"
	if (-not $icRaw.Contains($anchorUnion)) { throw "moonlight: SS_TOUCH_PACKET union anchor missing" }
	$icRaw = $icRaw.Replace($anchorUnion,
		"        SS_TOUCH_PACKET touch;" + $ieol +
		"        SS_TOUCHPAD_PACKET touchpad; // HMRDP_TOUCHPAD" + $ieol +
		"        SS_TOUCHPAD_FRAME_PACKET touchpadFrame; // HMRDP_TOUCHPAD" + $ieol +
		"        SS_PEN_PACKET pen;")

	$anchorStatic = "static uint8_t currentPenButtonState;"
	if (-not $icRaw.Contains($anchorStatic)) { throw "moonlight: currentPenButtonState anchor missing" }
	$icRaw = $icRaw.Replace($anchorStatic,
		$anchorStatic + $ieol + "static uint8_t currentTouchpadButtonState; // HMRDP_TOUCHPAD")

	$anchorDefine = "#define TOUCH_EVENT_IS_BATCHABLE(x) ((x) == LI_TOUCH_EVENT_HOVER || (x) == LI_TOUCH_EVENT_MOVE)"
	if (-not $icRaw.Contains($anchorDefine)) { throw "moonlight: TOUCH_EVENT_IS_BATCHABLE anchor missing" }
	$icRaw = $icRaw.Replace($anchorDefine,
		$anchorDefine + $ieol + "#define TOUCHPAD_FLUSH_INTERVAL_MS 1 // HMRDP_TOUCHPAD")

	$anchorInit = "    currentPenButtonState = 0;"
	if (-not $icRaw.Contains($anchorInit)) { throw "moonlight: currentPenButtonState init anchor missing" }
	$icRaw = $icRaw.Replace($anchorInit,
		$anchorInit + $ieol + "    currentTouchpadButtonState = 0; // HMRDP_TOUCHPAD")

	$anchorTime = "    uint64_t lastPenPacketTime = 0;"
	if (-not $icRaw.Contains($anchorTime)) { throw "moonlight: lastPenPacketTime anchor missing" }
	$icRaw = $icRaw.Replace($anchorTime,
		$anchorTime + $ieol + "    uint64_t lastTouchpadFlushTime = 0; // HMRDP_TOUCHPAD")

	# Pace touchpad frames: allow newer move-only frames to replace a queued one.
	$anchorSend = @(
		"        // Encrypt and send the input packet",
		"        if (!sendInputPacket(holder, LbqGetItemCount(&packetQueue) > 0)) {"
	) -join $ieol
	if (-not $icRaw.Contains($anchorSend)) { throw "moonlight: input send anchor missing" }
	$sendBlock = @(
		"        bool moreData = LbqGetItemCount(&packetQueue) > 0; // HMRDP_TOUCHPAD",
		"        if (holder->packet.header.magic == LE32(SS_TOUCHPAD_FRAME_MAGIC) && holder->enetPacketFlags == 0) {",
		"            uint64_t now = PltGetMillis();",
		"            if (moreData && now >= lastTouchpadFlushTime + TOUCHPAD_FLUSH_INTERVAL_MS) {",
		"                moreData = false;",
		"            }",
		"            if (!moreData) {",
		"                lastTouchpadFlushTime = now;",
		"            }",
		"}",
		"",
		"        // Encrypt and send the input packet",
		"        if (!sendInputPacket(holder, moreData)) {"
	) -join $ieol
	$icRaw = $icRaw.Replace($anchorSend, $sendBlock)

	# Public API appended at EOF (floatToNetfloat / freePacketHolder / queues are in scope).
	$api = @(
		"",
		"// HMRDP_TOUCHPAD: foundation-sunshine native precision touchpad.",
		"static uint16_t mlNormalizedToUint16(float in) {",
		"    if (in < 0.0f) { in = 0.0f; } else if (in > 1.0f) { in = 1.0f; }",
		"    return (uint16_t)(in * 65535.0f + 0.5f);",
		"}",
		"",
		"int LiSendTouchpadEvent(uint8_t eventType, uint32_t pointerId, float x, float y, float pressure,",
		"                        float contactAreaMajor, float contactAreaMinor, uint16_t rotation,",
		"                        uint16_t deviceWidthMm, uint16_t deviceHeightMm, uint8_t buttonState) {",
		"    PPACKET_HOLDER holder;",
		"    int err;",
		"    if (!initialized) {",
		"        return -2;",
		"    }",
		"    if (!(SunshineFeatureFlags & LI_FF_TOUCHPAD_EVENTS)) {",
		"        return LI_ERR_UNSUPPORTED;",
		"    }",
		"    holder = allocatePacketHolder(0);",
		"    if (holder == NULL) {",
		"        return -1;",
		"    }",
		"    holder->channelId = CTRL_CHANNEL_TOUCH;",
		"    holder->enetPacketFlags = (TOUCH_EVENT_IS_BATCHABLE(eventType) && !(buttonState ^ currentTouchpadButtonState)) ? 0 : ENET_PACKET_FLAG_RELIABLE;",
		"    currentTouchpadButtonState = buttonState;",
		"    holder->packet.touchpad.header.size = BE32(sizeof(SS_TOUCHPAD_PACKET) - sizeof(uint32_t));",
		"    holder->packet.touchpad.header.magic = LE32(SS_TOUCHPAD_MAGIC);",
		"    holder->packet.touchpad.eventType = eventType;",
		"    holder->packet.touchpad.buttonState = buttonState;",
		"    holder->packet.touchpad.rotation = LE16(rotation);",
		"    holder->packet.touchpad.pointerId = LE32(pointerId);",
		"    holder->packet.touchpad.deviceWidthMm = LE16(deviceWidthMm);",
		"    holder->packet.touchpad.deviceHeightMm = LE16(deviceHeightMm);",
		"    floatToNetfloat(x, holder->packet.touchpad.x);",
		"    floatToNetfloat(y, holder->packet.touchpad.y);",
		"    floatToNetfloat(pressure, holder->packet.touchpad.pressure);",
		"    floatToNetfloat(contactAreaMajor, holder->packet.touchpad.contactAreaMajor);",
		"    floatToNetfloat(contactAreaMinor, holder->packet.touchpad.contactAreaMinor);",
		"    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);",
		"    if (err != LBQ_SUCCESS) {",
		"        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);",
		"        Limelog(""Input queue reached maximum size limit\n"");",
		"        freePacketHolder(holder);",
		"    }",
		"    return err;",
		"}",
		"",
		"int LiSendTouchpadFrameEvent(uint8_t contactCount, const uint8_t* eventTypes, const uint32_t* pointerIds,",
		"                             const float* x, const float* y, const float* pressure, uint16_t rotation,",
		"                             uint16_t deviceWidthMm, uint16_t deviceHeightMm, uint8_t buttonState) {",
		"    PPACKET_HOLDER holder;",
		"    int err;",
		"    bool batchable = true;",
		"    uint8_t i;",
		"    if (!initialized) {",
		"        return -2;",
		"    }",
		"    if (!(SunshineFeatureFlags & LI_FF_TOUCHPAD_FRAME_EVENTS)) {",
		"        return LI_ERR_UNSUPPORTED;",
		"    }",
		"    if (contactCount > SS_TOUCHPAD_FRAME_MAX_CONTACTS ||",
		"        (contactCount > 0 && (eventTypes == NULL || pointerIds == NULL || x == NULL || y == NULL || pressure == NULL))) {",
		"        return -3;",
		"    }",
		"    holder = allocatePacketHolder(0);",
		"    if (holder == NULL) {",
		"        return -1;",
		"    }",
		"    holder->channelId = CTRL_CHANNEL_TOUCH;",
		"    holder->packet.touchpadFrame.header.size = BE32(sizeof(SS_TOUCHPAD_FRAME_PACKET) - sizeof(uint32_t));",
		"    holder->packet.touchpadFrame.header.magic = LE32(SS_TOUCHPAD_FRAME_MAGIC);",
		"    holder->packet.touchpadFrame.contactCount = contactCount;",
		"    holder->packet.touchpadFrame.buttonState = buttonState;",
		"    holder->packet.touchpadFrame.rotation = LE16(rotation);",
		"    holder->packet.touchpadFrame.deviceWidthMm = LE16(deviceWidthMm);",
		"    holder->packet.touchpadFrame.deviceHeightMm = LE16(deviceHeightMm);",
		"    memset(holder->packet.touchpadFrame.contacts, 0, sizeof(holder->packet.touchpadFrame.contacts));",
		"    for (i = 0; i < contactCount; i++) {",
		"        holder->packet.touchpadFrame.contacts[i].eventType = eventTypes[i];",
		"        holder->packet.touchpadFrame.contacts[i].pointerId = LE32(pointerIds[i]);",
		"        holder->packet.touchpadFrame.contacts[i].x = LE16(mlNormalizedToUint16(x[i]));",
		"        holder->packet.touchpadFrame.contacts[i].y = LE16(mlNormalizedToUint16(y[i]));",
		"        holder->packet.touchpadFrame.contacts[i].pressure = LE16(mlNormalizedToUint16(pressure[i]));",
		"        if (!TOUCH_EVENT_IS_BATCHABLE(eventTypes[i])) {",
		"            batchable = false;",
		"        }",
		"    }",
		"    holder->enetPacketFlags = (batchable && !(buttonState ^ currentTouchpadButtonState)) ? 0 : ENET_PACKET_FLAG_RELIABLE;",
		"    currentTouchpadButtonState = buttonState;",
		"    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);",
		"    if (err != LBQ_SUCCESS) {",
		"        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);",
		"        Limelog(""Input queue reached maximum size limit\n"");",
		"        freePacketHolder(holder);",
		"    }",
		"    return err;",
		"}"
	) -join $ieol
	$icRaw = $icRaw.TrimEnd() + $ieol + $api + $ieol

	[System.IO.File]::WriteAllText($InputStream, $icRaw)
	Write-Host "moonlight: added native touchpad send + packets to InputStream.c"
}

# 4) Clipboard sync (foundation-sunshine extension, control message 0x5508):
#    receive dispatch + callback, LiSendClipboardData, capability bits. Also
#    widens the control sender to carry 64 KiB frames (stock code uses a `short`
#    payload length and a 256-byte plaintext buffer).
$Limelight = Join-Path $MlSrc "Limelight.h"
$ControlStream = Join-Path $MlSrc "ControlStream.c"

# --- Limelight.h -------------------------------------------------------------
$hRaw = [System.IO.File]::ReadAllText($Limelight)
if ($hRaw.Contains("HMRDP_CLIPBOARD")) {
	Write-Host "moonlight: Limelight.h clipboard patch already applied (skipped)"
}
else {
	$heol = if ($hRaw.Contains("`r`n")) { "`r`n" } else { "`n" }

	$cbType = "typedef void(*ConnListenerClipboardData)(const char* data, int length); // HMRDP_CLIPBOARD"
	$anchorStruct = "typedef struct _CONNECTION_LISTENER_CALLBACKS {"
	if (-not $hRaw.Contains($anchorStruct)) { throw "moonlight: CONNECTION_LISTENER_CALLBACKS anchor missing" }
	$hRaw = $hRaw.Replace($anchorStruct, $cbType + $heol + $heol + $anchorStruct)

	$anchorField = "    ConnListenerCursorUpdate cursorUpdate; // HMRDP_CURSOR"
	if (-not $hRaw.Contains($anchorField)) { throw "moonlight: cursorUpdate field anchor missing" }
	$hRaw = $hRaw.Replace($anchorField, $anchorField + $heol +
		"    ConnListenerClipboardData clipboardData; // HMRDP_CLIPBOARD")

	$anchorProto = "void LiSetLocalCursorMode(bool enabled);"
	if (-not $hRaw.Contains($anchorProto)) { throw "moonlight: LiSetLocalCursorMode anchor missing" }
	$proto = @(
		$anchorProto,
		"",
		"// HMRDP_CLIPBOARD: foundation-sunshine clipboard sync (control message 0x5508).",
		"// `payload` is an opaque v1 wire frame forwarded verbatim by the host.",
		"// Returns 0 on success, -1 invalid/oversized, -2 unsupported, -3 not connected, -4 send failed.",
		"#define LI_CLIPBOARD_KIND_TEXT 1",
		"#define LI_CLIPBOARD_KIND_PNG  2",
		"#define LI_CLIPBOARD_KIND_REF  3",
		"int LiSendClipboardData(const void* payload, int length);"
	) -join $heol
	$hRaw = $hRaw.Replace($anchorProto, $proto)

	$anchorFF = "#define LI_FF_TOUCHPAD_FRAME_EVENTS   0x20 // LiSendTouchpadFrameEvent() supported // HMRDP_TOUCHPAD"
	if (-not $hRaw.Contains($anchorFF)) { throw "moonlight: LI_FF touchpad anchor missing" }
	$hRaw = $hRaw.Replace($anchorFF, $anchorFF + $heol +
		"#define LI_FF_CLIPBOARD_TEXT  0x04 // Clipboard text sync // HMRDP_CLIPBOARD" + $heol +
		"#define LI_FF_CLIPBOARD_IMAGE 0x08 // Clipboard image sync // HMRDP_CLIPBOARD")

	[System.IO.File]::WriteAllText($Limelight, $hRaw)
	Write-Host "moonlight: added clipboard API/feature bits to Limelight.h"
}

# --- ControlStream.c ---------------------------------------------------------
$cRaw = [System.IO.File]::ReadAllText($ControlStream)
if ($cRaw.Contains("HMRDP_CLIPBOARD")) {
	Write-Host "moonlight: ControlStream.c clipboard patch already applied (skipped)"
}
else {
	$ceol = if ($cRaw.Contains("`r`n")) { "`r`n" } else { "`n" }

	# Widen the payload length on every control sender so a 64 KiB frame fits.
	$cRaw = $cRaw.Replace("short ptype, short paylen", "short ptype, int paylen")

	$oldDecl = @(
		"        PNVCTL_ENCRYPTED_PACKET_HEADER encPacket;",
		"        PNVCTL_ENET_PACKET_HEADER_V2 packet;",
		"        char tempBuffer[256];"
	) -join $ceol
	$newDecl = @(
		"        PNVCTL_ENCRYPTED_PACKET_HEADER encPacket;",
		"        PNVCTL_ENET_PACKET_HEADER_V2 packet;",
		"        char tempBufferStack[256]; // HMRDP_CLIPBOARD",
		"        char* tempBuffer = tempBufferStack; // HMRDP_CLIPBOARD",
		"        const size_t plaintextLen = sizeof(*packet) + (size_t)paylen; // HMRDP_CLIPBOARD"
	) -join $ceol
	if (-not $cRaw.Contains($oldDecl)) { throw "moonlight: ControlStream tempBuffer anchor missing" }
	$cRaw = $cRaw.Replace($oldDecl, $newDecl)

	$oldCreate = @(
		"        enetPacket = enet_packet_create(NULL,",
		"                                        sizeof(*encPacket) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen,",
		"                                        flags);",
		"        if (enetPacket == NULL) {",
		"            return false;",
		"        }"
	) -join $ceol
	$newCreate = @(
		"        // HMRDP_CLIPBOARD: clipboard frames can be far larger than the stack buffer.",
		"        if (plaintextLen > sizeof(tempBufferStack)) {",
		"            tempBuffer = (char*)malloc(plaintextLen);",
		"            if (tempBuffer == NULL) {",
		"                return false;",
		"            }",
		"        }",
		"",
		"        enetPacket = enet_packet_create(NULL,",
		"                                        sizeof(*encPacket) + AES_GCM_TAG_LENGTH + plaintextLen,",
		"                                        flags);",
		"        if (enetPacket == NULL) {",
		"            if (tempBuffer != tempBufferStack) {",
		"                free(tempBuffer);",
		"            }",
		"            return false;",
		"        }"
	) -join $ceol
	if (-not $cRaw.Contains($oldCreate)) { throw "moonlight: ControlStream packet-create anchor missing" }
	$cRaw = $cRaw.Replace($oldCreate, $newCreate)

	$oldLen = "        encPacket->length = sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen;"
	$newLen = "        encPacket->length = (unsigned short)(sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + plaintextLen); // HMRDP_CLIPBOARD"
	if (-not $cRaw.Contains($oldLen)) { throw "moonlight: ControlStream length anchor missing" }
	$cRaw = $cRaw.Replace($oldLen, $newLen)

	$oldFill = @(
		"        LC_ASSERT(sizeof(*packet) + paylen < sizeof(tempBuffer));",
		"        packet = (PNVCTL_ENET_PACKET_HEADER_V2)tempBuffer;",
		"        packet->type = ptype;",
		"        packet->payloadLength = paylen;",
		"        memcpy(&packet[1], payload, paylen);"
	) -join $ceol
	$newFill = @(
		"        packet = (PNVCTL_ENET_PACKET_HEADER_V2)tempBuffer; // HMRDP_CLIPBOARD",
		"        packet->type = ptype;",
		"        packet->payloadLength = (unsigned short)paylen; // HMRDP_CLIPBOARD",
		"        memcpy(&packet[1], payload, (size_t)paylen); // HMRDP_CLIPBOARD"
	) -join $ceol
	if (-not $cRaw.Contains($oldFill)) { throw "moonlight: ControlStream plaintext fill anchor missing" }
	$cRaw = $cRaw.Replace($oldFill, $newFill)

	$oldFail = @(
		"        if (!encryptControlMessage(encPacket, packet)) {",
		"            Limelog(""Failed to encrypt control stream message\n"");",
		"            enet_packet_destroy(enetPacket);",
		"            PltUnlockMutex(&enetMutex);",
		"            return false;",
		"        }"
	) -join $ceol
	$newFail = @(
		"        if (!encryptControlMessage(encPacket, packet)) {",
		"            Limelog(""Failed to encrypt control stream message\n"");",
		"            enet_packet_destroy(enetPacket);",
		"            if (tempBuffer != tempBufferStack) { // HMRDP_CLIPBOARD",
		"                free(tempBuffer);",
		"            }",
		"            PltUnlockMutex(&enetMutex);",
		"            return false;",
		"        }",
		"        if (tempBuffer != tempBufferStack) { // HMRDP_CLIPBOARD",
		"            free(tempBuffer);",
		"        }"
	) -join $ceol
	if (-not $cRaw.Contains($oldFail)) { throw "moonlight: ControlStream encrypt anchor missing" }
	$cRaw = $cRaw.Replace($oldFail, $newFail)

	# Message constant + receive dispatch (before the cursor branch).
	$anchorConst = "#define ML_CURSOR_HEADER_SIZE 28"
	if (-not $cRaw.Contains($anchorConst)) { throw "moonlight: cursor header-size anchor missing" }
	$cRaw = $cRaw.Replace($anchorConst,
		$anchorConst + $ceol + "#define ML_CLIPBOARD_CTRL_TYPE 0x5508 // HMRDP_CLIPBOARD")

	$anchorDispatch = "            else if (ctlHdr->type == ML_CURSOR_CTRL_TYPE) { // HMRDP_CURSOR"
	if (-not $cRaw.Contains($anchorDispatch)) { throw "moonlight: cursor dispatch anchor missing" }
	$clipDispatch = @(
		"            else if (ctlHdr->type == ML_CLIPBOARD_CTRL_TYPE) { // HMRDP_CLIPBOARD",
		"                // Opaque v1 frame; the client must not block this thread.",
		"                const char* clipPayload = (const char*)(ctlHdr + 1);",
		"                int clipLen = packetLength - (int)sizeof(*ctlHdr);",
		"                if (ListenerCallbacks.clipboardData != NULL && clipLen > 0) {",
		"                    ListenerCallbacks.clipboardData(clipPayload, clipLen);",
		"                }",
		"            }",
		$anchorDispatch
	) -join $ceol
	$cRaw = $cRaw.Replace($anchorDispatch, $clipDispatch)

	# Sender at EOF (sendMessageAndForget is in scope there).
	$api = @(
		"",
		"// HMRDP_CLIPBOARD: foundation-sunshine clipboard sync (control message 0x5508).",
		"int LiSendClipboardData(const void* payload, int length) {",
		"    // 65511 leaves room for the 24-byte encrypted control-frame overhead.",
		"    if (payload == NULL || length <= 0 || length > 65511) {",
		"        return -1;",
		"    }",
		"    if (!IS_SUNSHINE() || !encryptedControlStream) {",
		"        return -2;",
		"    }",
		"    if (peer == NULL || peer->state != ENET_PEER_STATE_CONNECTED) {",
		"        return -3;",
		"    }",
		"    if (!sendMessageAndForget(ML_CLIPBOARD_CTRL_TYPE, length, payload,",
		"                              CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false)) {",
		"        return -4;",
		"    }",
		"    return 0;",
		"}"
	) -join $ceol
	$cRaw = $cRaw.TrimEnd() + $ceol + $api + $ceol

	[System.IO.File]::WriteAllText($ControlStream, $cRaw)
	Write-Host "moonlight: added clipboard message handling to ControlStream.c"
}

Write-Host "=== moonlight-common-c patches applied ==="
