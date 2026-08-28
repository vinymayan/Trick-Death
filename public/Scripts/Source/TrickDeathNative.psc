Scriptname TrickDeathNative Hidden

; Respawn and policy masks:
; RespawnHere=1, LastSleep=2, LastCheckpoint=4,
; ReloadSave=8 (legacy name: LoadLastSave), DisableTrickDeath=16.

Bool Function SetCheckpoint(ObjectReference anchor, Form owner, String name = "", Int blockedRespawns = 0) Global Native
Bool Function SetCheckpointAtPlayer(Form owner, String name = "", Int blockedRespawns = 0) Global Native
Bool Function ClearCheckpoint(Form owner) Global Native
Bool Function HasCheckpoint() Global Native
Bool Function HasLastSleep() Global Native

; area may be None (global), a Cell, or a Location. Any blocking policy wins.
Bool Function SetRespawnPolicy(Form owner, Form area, Int blockedMask, Bool persistent = False) Global Native
Bool Function ClearRespawnPolicy(Form owner, Form area) Global Native
Int Function ClearRespawnPolicies(Form owner) Global Native

; Slots: title (or defeated), background_text (or background/death_message), respawn_here, respawn_last_sleep,
; respawn_checkpoint, reload_save (legacy alias: load_last_save).
; Variables accept {$key} and {{$key}}. Unknown variables remain unchanged.
Bool Function SetTextOverride(Form owner, String slot, String textTemplate, Int priority = 0, Bool persistent = False) Global Native
Bool Function SetTextVariable(Form owner, String key, String value, Bool persistent = False) Global Native
Int Function ClearTextOverrides(Form owner) Global Native
Int Function GetAvailableRespawns() Global Native

; Mod events emitted by this plugin:
; TrickDeath_LastSleepChanged, TrickDeath_CheckpointChanged,
; TrickDeath_RespawnSelected, TrickDeath_RespawnCompleted.
