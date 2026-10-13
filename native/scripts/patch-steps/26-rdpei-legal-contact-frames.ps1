# 26) Keep every RDPEI touch frame protocol-legal: a frame carries a contact once, so a move/lift
#     landing with the DOWN must not collapse the report the remote needs to engage the contact.
$rdpeiHeader = "$Source\channels\rdpei\client\rdpei_main.h"
$rdpeiMain = "$Source\channels\rdpei\client\rdpei_main.c"

$marker = 'HmRdp: legal contact frames'
$headerMarker = 'HmRdp: the remote has been told this contact is ENGAGED'
$resetMarker = 'contactPoint->engaged = FALSE;'

if (-not [System.IO.File]::ReadAllText($rdpeiHeader).Contains($headerMarker)) {
  $hdrNew = @(
    'BOOL active;',
    (Tabs 1 ('/* ' + $headerMarker + ' (its DOWN went out); keeps the reported transitions legal. */')),
    (Tabs 1 'BOOL engaged;'),
    (Tabs 1 'UINT32 contactId;')
  ) -join "`n"
  Patch-Regex -Path $rdpeiHeader -Marker "" `
    -Pattern 'BOOL\s+active;\s*\n\s*UINT32 contactId;' -Replacement $hdrNew
  Write-Host "HmRdp RDPEI contact point flag added to rdpei_main.h"
}

if (-not [System.IO.File]::ReadAllText($rdpeiMain).Contains($resetMarker)) {
  $resetNew = @(
    'contactPoint->externalId = externalId;',
    (Tabs 3 'contactPoint->engaged = FALSE;'),
    (Tabs 3 'contactPoint->active = TRUE;')
  ) -join "`n"
  Patch-Regex -Path $rdpeiMain -Marker "" `
    -Pattern 'contactPoint->externalId = externalId;\s*\n\s*contactPoint->active = TRUE;' `
    -Replacement $resetNew
  Write-Host "HmRdp RDPEI contact point reset added to rdpei_main.c"
}

if (-not [System.IO.File]::ReadAllText($rdpeiMain).Contains($marker)) {
  $loopNew = @(
    'RDPINPUT_CONTACT_DATA* contact = &contactPoint->data;',
    (Tabs 2 ('/* ' + $marker + ' - a frame carries a contact once: the app''s moves/lifts are merged into one legal report; a pre-DOWN lift is deferred a frame. */')),
    (Tabs 2 'if (!contactPoint->active && !contactPoint->dirty)'),
    (Tabs 3 'continue;'),
    '',
    (Tabs 2 'RDPINPUT_CONTACT_DATA out = *contact;'),
    (Tabs 2 'const BOOL lifted = (contact->contactFlags & RDPINPUT_CONTACT_FLAG_UP) != 0;'),
    (Tabs 2 'BOOL release = FALSE;'),
    '',
    (Tabs 2 'if (contactPoint->active && !contactPoint->engaged)'),
    (Tabs 2 '{'),
    (Tabs 3 'out.contactFlags = RDPINPUT_CONTACT_FLAG_DOWN | RDPINPUT_CONTACT_FLAG_INRANGE |'),
    (Tabs 4 'RDPINPUT_CONTACT_FLAG_INCONTACT;'),
    (Tabs 3 'contactPoint->engaged = TRUE;'),
    (Tabs 2 '}'),
    (Tabs 2 'else if (contactPoint->active && lifted)'),
    (Tabs 2 '{'),
    (Tabs 3 'out.contactFlags = RDPINPUT_CONTACT_FLAG_UP;'),
    (Tabs 3 'release = TRUE;'),
    (Tabs 2 '}'),
    (Tabs 2 'else if (contactPoint->active)'),
    (Tabs 2 '{'),
    (Tabs 3 'out.contactFlags = RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE |'),
    (Tabs 4 'RDPINPUT_CONTACT_FLAG_INCONTACT;'),
    (Tabs 2 '}'),
    '',
    (Tabs 2 'contacts[frame.contactCount] = out;'),
    (Tabs 2 'frame.contactCount++;'),
    '',
    (Tabs 2 'if (release)'),
    (Tabs 2 '{'),
    (Tabs 3 'contactPoint->dirty = FALSE;'),
    (Tabs 3 'contactPoint->engaged = FALSE;'),
    (Tabs 3 'contactPoint->active = FALSE;'),
    (Tabs 3 'contactPoint->externalId = 0;'),
    (Tabs 3 'contactPoint->contactId = 0;'),
    (Tabs 2 '}'),
    (Tabs 2 'else'),
    (Tabs 2 '{'),
    (Tabs 3 '/* The DOWN is out; stay dirty only for a lift that predates it. */'),
    (Tabs 3 'contactPoint->dirty = contactPoint->active && lifted;'),
    (Tabs 2 '}')
  ) -join "`n"
  Patch-Regex -Path $rdpeiMain -Marker "" `
    -Pattern 'RDPINPUT_CONTACT_DATA\* contact = &contactPoint->data;[\s\S]*?contactPoint->contactId = 0;\s*\n\s*\}' `
    -Replacement $loopNew
  Write-Host "HmRdp RDPEI legal frame merge applied to rdpei_main.c"
}
