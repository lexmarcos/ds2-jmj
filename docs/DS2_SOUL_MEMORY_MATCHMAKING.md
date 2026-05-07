# Possible Changes: DS2 Soul Memory Matchmaking

This document records the Soul Memory matchmaking findings for Dark Souls II and lists possible changes for opening matchmaking on the private server.

## Current behavior

Dark Souls II uses Soul Memory as part of its matchmaking rules.

Observed tests:

- Player A with `soul_memory=3751200` could not see a Red Soapstone sign from Player B with `soul_memory=400`.
- When Player B was changed to `soul_memory=3750400`, Player A received `response_sign_count=1`.
- Summoning worked after Soul Memory became compatible.

This confirms that the server-side DS2 matchmaking path is enforcing Soul Memory tiers for Red Soapstone signs.

## Relevant player state

The DS2 player state exposes Soul Memory through:

```cpp
uint32 DS2_PlayerState::GetSoulMemory()
{
    return GetPlayerStatus().player_status().soul_memory();
}
```

This value is then used by the matchmaking parameter checks.

## Soul Memory tier logic

Soul Memory tier checks are controlled by `RuntimeConfigSoulMemoryMatchingParameters`.

Important fields:

- `Tiers`
- `DisableSoulMemoryMatching`
- `TiersBelow`
- `TiersAbove`
- password variants such as wider tier ranges or disabled Soul Memory matching

When `DisableSoulMemoryMatching` is false, the server compares each player's Soul Memory tier and applies the configured tier range.

## Matchmaking locations

The main DS2 matchmaking paths are:

- Summon signs: `DS2_SignManager::CanMatchWith`
- Red Soapstone signs: `DS2_RedSoapstoneMatchingParameters`
- Invasions and orbs: `DS2_BreakInManager::CanMatchWith`
- Covenant auto-summon / visitor flows: `DS2_VisitorManager::CanMatchWith`
- Arena / quick match: `DS2_QuickMatchManager::CanMatchWith`

## Easiest MVP change

For Red Soapstone only, the smallest test is config-only:

```json
"DS2_RedSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
}
```

This should keep the change scoped to Red Soapstone signs while leaving other PvP systems unchanged.

## Wider DS2 opening

A broader DS2 matchmaking opening would involve all DS2 matching parameter blocks, including:

- `DS2_WhiteSoapstoneMatchingParameters`
- `DS2_SmallWhiteSoapstoneMatchingParameters`
- `DS2_RedSoapstoneMatchingParameters`
- `DS2_DragonSoapstoneMatchingParameters`
- orb / break-in matching parameters
- covenant auto-summon matching parameters
- quick match / arena matching parameters

The safer approach is to open one path at a time, starting with Red Soapstone.

## Alternative: global flag

Another option is a global server config flag such as:

```json
"DS2OpenSoulMemoryMatchmaking": true
```

This would bypass Soul Memory checks in all DS2 matching paths.

That is simpler operationally but riskier because it changes more systems at once.

## Risks

- Opening Soul Memory too broadly can affect all PvP and summon flows.
- Password matching may already have special tier behavior; do not accidentally regress it.
- Some matching paths may rely on Soul Memory for balance or expected client behavior.
- The client UI may still show restrictions even if the server allows the match.

## Recommended plan

1. Add a config-only override for `DS2_RedSoapstoneMatchingParameters`.
2. Test Red Soapstone visibility with players in very different Soul Memory tiers.
3. Confirm summon creation and join still work.
4. Only then decide whether to expose a wider global DS2 setting.
