# 26) HmRdp: keep every RDPEI touch frame protocol-legal.
#
#     A touch frame carries at most one report per contact, so whatever the app
#     produced inside one ~8/20ms window is coalesced into that single report
#     (step 6 exposes the window). Upstream keeps only the contact's latest flags,
#     which breaks the MS-RDPEI state machine as soon as a move - or a lift - lands
#     in the same window as the DOWN: the first report the remote sees for that
#     contact then becomes UPDATE|INRANGE|INCONTACT, and the only transition into
#     ENGAGED is DOWN|INRANGE|INCONTACT (see the state table in
#     channels/rdpei/client/rdpei_main.h). The remote never engages the contact, so
#     a hold there never turns into a press-and-hold (no right-click menu), while
#     the touch visual it drew for the malformed contact is left behind and later
#     presses pile more of them up. It bites long presses first because a held
#     finger produces moves immediately, i.e. inside the window that follows DOWN.
#
#     The merge itself stays: the emitted report is chosen so the transition is
#     always legal. A contact whose DOWN has not gone out yet is reported as DOWN,
#     with the latest coordinates/fields (so nothing the app produced is lost to
#     the merge); a lift that arrived before that DOWN went out is kept pending for
#     the next frame instead of collapsing into a UP the remote could not apply; a
#     contact that is already engaged keeps the old behaviour (UPDATE while held,
#     UP to release). One flag per contact records that its DOWN went out; it lives
#     in the channel-private rdpei_main.h, so the public headers the app compiles
#     against are untouched (nothing to re-sync).
$rdpeiHeader = "$Source\channels\rdpei\client\rdpei_main.h"
$rdpeiMain = "$Source\channels\rdpei\client\rdpei_main.c"

$marker = 'HmRdp: legal contact frames'
$headerMarker = 'HmRdp: the remote has been told this contact is ENGAGED'
$resetMarker = 'contactPoint->engaged = FALSE;'

# The flag on the contact point (channel-private struct).
if (-not [System.IO.File]::ReadAllText($rdpeiHeader).Contains($headerMarker)) {
  $hdrNew = @(
    'BOOL active;',
    (Tabs 1 ('/* ' + $headerMarker + ' (its DOWN')),
    (Tabs 1 ' * went out). A frame carries a contact once, so the reported transitions'),
    (Tabs 1 ' * are kept legal against this flag (see rdpei_add_frame). */'),
    (Tabs 1 'BOOL engaged;'),
    (Tabs 1 'UINT32 contactId;')
  ) -join "`n"
  Patch-Regex -Path $rdpeiHeader -Marker "" `
    -Pattern 'BOOL\s+active;\s*\n\s*UINT32 contactId;' -Replacement $hdrNew
  Write-Host "HmRdp RDPEI contact point flag added to rdpei_main.h"
}

# A (re)used slot starts out of range again.
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

# The frame merge itself.
if (-not [System.IO.File]::ReadAllText($rdpeiMain).Contains($marker)) {
  $loopNew = @(
    'RDPINPUT_CONTACT_DATA* contact = &contactPoint->data;',
    (Tabs 2 ('/* ' + $marker + ' - a frame carries a contact once, so what')),
    (Tabs 2 ' * the app produced in one window is merged into a single legal report: a'),
    (Tabs 2 ' * contact whose DOWN has not gone out yet is reported as DOWN here (MS-RDPEI'),
    (Tabs 2 ' * can only enter INRANGE|INCONTACT through DOWN), with the latest'),
    (Tabs 2 ' * coordinates/fields so the merge loses nothing; a lift that arrived before'),
    (Tabs 2 ' * that DOWN is deferred one frame instead of becoming an UP the remote'),
    (Tabs 2 ' * cannot apply. */'),
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
