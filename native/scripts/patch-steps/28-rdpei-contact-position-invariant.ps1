# 28) The frame that releases a contact must not move it: leaving ENGAGED must not change position,
#     so the merged UP reports the position the remote last saw (otherwise the remote keeps it ENGAGED).
$rdpeiHeader = "$Source\channels\rdpei\client\rdpei_main.h"
$rdpeiMain = "$Source\channels\rdpei\client\rdpei_main.c"

$headerMarker = 'HmRdp: the position the remote last saw'
$mainMarker = 'contactPoint->lastX'

if (-not [System.IO.File]::ReadAllText($rdpeiHeader).Contains($headerMarker)) {
  $hdrNew = @(
    'BOOL engaged;',
    (Tabs 1 ('/* ' + $headerMarker + ' for this contact; the UP must not move it. */')),
    (Tabs 1 'INT32 lastX;'),
    (Tabs 1 'INT32 lastY;'),
    (Tabs 1 'UINT32 contactId;')
  ) -join "`n"
  Patch-Regex -Path $rdpeiHeader -Marker "" `
    -Pattern 'BOOL engaged;\s*\n\s*UINT32 contactId;' -Replacement $hdrNew
  Write-Host "HmRdp RDPEI last reported position added to rdpei_main.h"
}

if (-not [System.IO.File]::ReadAllText($rdpeiMain).Contains($mainMarker)) {
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

  $upNew = @(
    'else if (contactPoint->active && lifted)',
    (Tabs 2 '{'),
    (Tabs 3 '/* The moves carrying the lift position are in this same merged frame, so the UP reports the remote''s last-seen position. */'),
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
