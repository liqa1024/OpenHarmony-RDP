# 28) HmRdp: the frame that releases a contact must not move it.
#
#     rdpei_main.h "Touch Contact State Transitions" is explicit: leaving ENGAGED
#     (UP -> OUT_OF_RANGE) must not change the contact position, it may only move
#     after the transition. freerdp_handle_touch_up respects that by sending an
#     UPDATE at the lift position first and the UP at the same position after it -
#     two separate reports, so the move is legal (ENGAGED -> ENGAGED) and the UP
#     then leaves the position alone.
#
#     A frame carries a contact once (step 26), so when those two reports land in
#     the same window only the UP survives and it now carries a position the remote
#     never saw -> the transition moves the contact -> the remote does not apply it
#     and the contact stays ENGAGED (its touch visual never goes away). Whether the
#     two collapse depends on a flush interleaving between them, which is why the
#     failure is intermittent and shifts when anything perturbs the timing.
#
#     The merge keeps collapsing; the UP just reports the position the remote last
#     saw for that contact. That position is already the remote's, so the release
#     moves nothing and the transition is legal. A contact the remote has not seen
#     at all is reported as DOWN first (step 26), and the deferred UP it defers
#     uses that same position.
$rdpeiHeader = "$Source\channels\rdpei\client\rdpei_main.h"
$rdpeiMain = "$Source\channels\rdpei\client\rdpei_main.c"

$headerMarker = 'HmRdp: the position the remote last saw'
$mainMarker = 'contactPoint->lastX'

if (-not [System.IO.File]::ReadAllText($rdpeiHeader).Contains($headerMarker)) {
  $hdrNew = @(
    'BOOL engaged;',
    (Tabs 1 ('/* ' + $headerMarker + ' for this contact; a frame that')),
    (Tabs 1 ' * carries the UP must not move it (see the state table above). */'),
    (Tabs 1 'INT32 lastX;'),
    (Tabs 1 'INT32 lastY;'),
    (Tabs 1 'UINT32 contactId;')
  ) -join "`n"
  Patch-Regex -Path $rdpeiHeader -Marker "" `
    -Pattern 'BOOL engaged;\s*\n\s*UINT32 contactId;' -Replacement $hdrNew
  Write-Host "HmRdp RDPEI last reported position added to rdpei_main.h"
}

if (-not [System.IO.File]::ReadAllText($rdpeiMain).Contains($mainMarker)) {
  # Remember the position of every report that is actually sent.
  $downNew = @(
    'contactPoint->engaged = TRUE;',
    (Tabs 3 'contactPoint->lastX = out.x;'),
    (Tabs 3 'contactPoint->lastY = out.y;')
  ) -join "`n"
  Patch-Regex -Path $rdpeiMain -Marker "" -Pattern 'contactPoint->engaged = TRUE;' `
    -Replacement $downNew

  $updateNew = @(
    'else if (contactPoint->active)',
    (Tabs 2 '{'),
    (Tabs 3 'out.contactFlags = RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE |'),
    (Tabs 4 'RDPINPUT_CONTACT_FLAG_INCONTACT;'),
    (Tabs 3 'contactPoint->lastX = out.x;'),
    (Tabs 3 'contactPoint->lastY = out.y;'),
    (Tabs 2 '}')
  ) -join "`n"
  Patch-Regex -Path $rdpeiMain -Marker "" `
    -Pattern 'else if \(contactPoint->active\)\s*\n\s*\{\s*\n\s*out\.contactFlags = RDPINPUT_CONTACT_FLAG_UPDATE \| RDPINPUT_CONTACT_FLAG_INRANGE \|\s*\n\s*RDPINPUT_CONTACT_FLAG_INCONTACT;\s*\n\s*\}' `
    -Replacement $updateNew

  # The release itself carries the position the remote already has.
  $upNew = @(
    'else if (contactPoint->active && lifted)',
    (Tabs 2 '{'),
    (Tabs 3 '/* The moves that would have carried the lift position are in this same'),
    (Tabs 3 ' * merged frame, so the UP reports the position the remote last saw. */'),
    (Tabs 3 'out.x = contactPoint->lastX;'),
    (Tabs 3 'out.y = contactPoint->lastY;'),
    (Tabs 3 'out.contactFlags = RDPINPUT_CONTACT_FLAG_UP;'),
    (Tabs 3 'release = TRUE;'),
    (Tabs 2 '}')
  ) -join "`n"
  Patch-Regex -Path $rdpeiMain -Marker "" `
    -Pattern 'else if \(contactPoint->active && lifted\)\s*\n\s*\{\s*\n\s*out\.contactFlags = RDPINPUT_CONTACT_FLAG_UP;\s*\n\s*release = TRUE;\s*\n\s*\}' `
    -Replacement $upNew

  Write-Host "HmRdp RDPEI contact position invariant applied to rdpei_main.c"
}
