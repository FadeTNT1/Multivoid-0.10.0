// ue_wrap/core/sdk_profile_names.h -- the version surface, part 2: content names.
//
// Everything here is a game-content identity (blueprint class names, UFunction names, level names,
// save-landmark coordinates): it changes when the game's blueprints or maps change, not when the
// engine recompiles. Re-derive from the CXX header dump or bp_reflect on a game-version bump.
// sdk_profile.h includes this header, so consumers keep one include and the P::name:: spelling.

#pragma once

namespace ue_wrap::profile {

// ---- content names (change with game content, not the engine) ------------
namespace name {
inline constexpr const wchar_t* MainPlayerClass = L"mainPlayer_C";
inline constexpr const wchar_t* GamemodeClass = L"mainGamemode_C";
// AmainGamemode_C::transition(FName LevelName): the game's own level-travel verb;
// ReturnToMainMenu calls it with the full path "/Game/menu" (the short name does not resolve).
inline constexpr const wchar_t* MainGamemodeTransitionFn = L"transition";
inline constexpr const wchar_t* ActorClass = L"Actor";
inline constexpr const wchar_t* WorldClass = L"World";
inline constexpr const wchar_t* SetActorLocationFn = L"K2_SetActorLocation";
inline constexpr const wchar_t* GameplayLevel = L"untitled_1";

// The checkpoint spawn point at the base entrance (the actor centre, world cm; re-derive per game
// version): every client appearance is teleported here (coop/session/teleport_client).
inline constexpr float kKPPSpawnX = -37695.f;
inline constexpr float kKPPSpawnY =  69978.f;
inline constexpr float kKPPSpawnZ =   6420.f;

// Engine classes and functions dispatched through (stable engine names, kept here so the porting
// surface is one file). The persistent GameInstance subclass is game content (the world context
// that survives a level load).
inline constexpr const wchar_t* KismetSystemLibraryClass = L"KismetSystemLibrary";
inline constexpr const wchar_t* ExecuteConsoleCommandFn = L"ExecuteConsoleCommand";
inline constexpr const wchar_t* GameInstanceClass = L"mainGameInstance_C";
// The "apply the save on BeginPlay" flag the save menu's Launch sets: loadObjectsFromSlot on 0.10.0,
// loadObjects before it.
inline constexpr const wchar_t* GameInstanceLoadObjects       = L"loadObjectsFromSlot";
inline constexpr const wchar_t* GameInstanceLoadObjectsLegacy = L"loadObjects";

// The live UsaveSlot_C instance holds the player's current runtime vitals (food, sleep, health,
// coffeePower), which the BP mutates in place and SaveGameToSlot serialises whole; its field
// offsets are resolved at runtime through FindPropertyOffset (a BP-cooked class).
inline constexpr const wchar_t* SaveSlotClass = L"saveSlot_C";

// 0.10.0 moved the survival floats off saveSlot_C (its health/maxHealth/food/sleep/battery/
// coffeePower/gasolinepilled are now dead `obsoleteVar_*`) onto the PAWN, mainPlayer_C, as `SAVE_*`
// properties (the Blueprint bytecode of mainPlayer_C's damage writer, `add player damage`, reads
// and writes SAVE_health / SAVE_maxHealth on the pawn). strength/agility stayed on saveSlot_C.
// There is no battery property on the pawn in the 0.10.0 capture: that row stays unresolved.
// hasGlasses is now the Bool `equipped_glasses`. FindPropertyOffset is case-insensitive.
inline constexpr const wchar_t* PawnHealth         = L"SAVE_health";
inline constexpr const wchar_t* PawnMaxHealth      = L"SAVE_maxHealth";
inline constexpr const wchar_t* PawnFood           = L"SAVE_food";
inline constexpr const wchar_t* PawnSleep          = L"SAVE_sleep";
inline constexpr const wchar_t* PawnBattery        = L"SAVE_battery";  // absent in 0.10.0
inline constexpr const wchar_t* PawnCoffeePower    = L"SAVE_coffeePower";
inline constexpr const wchar_t* PawnGasolinePilled = L"SAVE_gasolinepilled";
inline constexpr const wchar_t* PawnGlasses        = L"equipped_glasses";

// 0.10.0's starter loadout. A new save is born with an empty startupEquipment map; the game's own
// create (lib_C::createSaveSlotTreeDirectory) copies defaultStartupEquipment into it, and the
// player's inventory screen spawns those items into the equipment slots on the first world
// (saveSlot_C::addStartupEquipment), then clears the map. reset_player does the same copy, plus a
// reset of playerTransform to def (the caller keeps and restores it) and a clear of betterObjectsData.
inline constexpr const wchar_t* SaveSlotStartupEquipment        = L"startupEquipment";
inline constexpr const wchar_t* SaveSlotDefaultStartupEquipment = L"defaultStartupEquipment";
inline constexpr const wchar_t* SaveSlotResetPlayerFn           = L"reset_player";
inline constexpr const wchar_t* SaveSlotPlayerTransform         = L"playerTransform";
inline constexpr const wchar_t* SaveSlotPlayerTransformDefault  = L"def";

// 0.10.0's world arrays on saveSlot_C.
inline constexpr const wchar_t* SaveSlotBetterObjectsData      = L"betterObjectsData";
inline constexpr const wchar_t* SaveSlotBetterPrimitivesData    = L"betterPrimitivesData";
inline constexpr const wchar_t* SaveSlotBetterTriggerData       = L"betterTriggerData";
// 0.9.0n's world object array (TArray<Fstruct_save>); 0.10.0 gathers into betterObjectsData instead.
inline constexpr const wchar_t* SaveSlotObjectsDataLegacy       = L"objectsData";
inline constexpr const wchar_t* SaveSlotPrimitivesDataLegacy    = L"primitivesData";

// The gamemode's live save object. 0.10.0 renamed the member saveSlot -> save_slot; both are tried,
// the current name first, so a reader resolves it on either build.
inline constexpr const wchar_t* GamemodeSaveSlotProp       = L"save_slot";
inline constexpr const wchar_t* GamemodeSaveSlotPropLegacy = L"saveSlot";
// mainGamemode_C::car: the world's ATV, the one the atvExplode special sets `trap` on.
inline constexpr const wchar_t* GamemodeCarProp            = L"car";

// The per-item save record struct (a BP UserDefinedStruct): struct_save1 on 0.10.0, struct_save
// before. Its layout is MEASURED through reflection (ue_wrap/actors/save_record); the names are only
// the fallback route to the struct when prop_C::getData has not loaded yet.
inline constexpr const wchar_t* SaveRecordStruct       = L"struct_save1";
inline constexpr const wchar_t* SaveRecordStructLegacy = L"struct_save";
// 0.10.0's better-save record (struct_betterSave): what saveObjects gathers into betterObjectsData.
inline constexpr const wchar_t* BetterSaveStruct = L"struct_betterSave";

// The dynamic signal row (desk refiner, drive, rack, signal library). 0.10.0 added a
// signalObjectImageName member mid-struct; its layout is measured (ue_wrap/desk/signal_dynamic.h).
inline constexpr const wchar_t* SignalDynamicStruct = L"struct_signalDataDynamic";

// The drive box (Aprop_box_C, the "data case"): `drives` is its TArray of signal rows (one per slot,
// copied from the drive's data_0 when it is put in), `drives_in` its TArray<bool> occupancy, and upd() the
// repaint that reads both. 0.10.0's getData/loadData carry the occupancy and NOT the rows
// (ue_wrap/actors/drive_bank).
inline constexpr const wchar_t* DriveBoxClass       = L"prop_box_C";
inline constexpr const wchar_t* DriveBoxDrivesProp  = L"drives";
inline constexpr const wchar_t* DriveBoxDrivesInProp = L"drives_in";
inline constexpr const wchar_t* DriveBoxUpdFn       = L"upd";

// 0.10.0's container inventory: prop_container_C.inventoryComponent (an inventoryComponent_C, the
// grid inventory that replaced propInventory_C + saveSlot.GObjStack), whose `owner` its BeginPlay
// (makeOwner) sets to the container -- the "this container has started" mark the host's world
// settle counts.
inline constexpr const wchar_t* ContainerInventoryComponentProp = L"inventoryComponent";
inline constexpr const wchar_t* InventoryComponentOwnerProp     = L"owner";

// 0.10.0's world storage (ue_wrap/actors/storage_inventory): every actor that stores items for anyone
// -- a prop_container_C and its subclasses (the drone's sack among them), the delivery drone_C, the
// ATV_C -- carries an inventoryComponent_C named as above. Its canvas array holds struct_inventoryItem
// rows (position, rotation, amount, compartment and a struct_betterSave `data`), read and written
// through the struct's own reflection (ue_wrap/actors/inventory_item_codec), and its own verbs add,
// remove, move and re-derive them. The player's inventoryComponent_player_C is a subclass of it and is
// never a world storage.
inline constexpr const wchar_t* InventoryComponentClass        = L"inventoryComponent_C";
inline constexpr const wchar_t* PlayerInventoryComponentClass  = L"inventoryComponent_player_C";
inline constexpr const wchar_t* InventorySlotArrayC            = L"SlotArray_C";
inline constexpr const wchar_t* InventorySlotArrayD            = L"SlotArray_D";
inline constexpr const wchar_t* InventoryMaxVolumeProp         = L"maxVolume";
inline constexpr const wchar_t* InventoryTotalVolumeProp       = L"totalVolume";
inline constexpr const wchar_t* InventoryInfiniteVolumeProp    = L"infiniteVolume";
inline constexpr const wchar_t* InventoryActiveCanvasProp      = L"activeDisplayCanvas";
inline constexpr const wchar_t* InventoryAddItemAdvancedFn     = L"addItemAdvanced";
inline constexpr const wchar_t* InventoryRemoveItemFn          = L"removeInventoryItem";
inline constexpr const wchar_t* InventoryTransferFn            = L"transferFromInvToInf";
inline constexpr const wchar_t* InventoryCalculateVolumesFn    = L"calculateVolumes";
inline constexpr const wchar_t* InventoryAssignCanvasFn        = L"assignCanvas";
inline constexpr const wchar_t* ContainerLockedProp            = L"locked";
inline constexpr const wchar_t* DroneClass                     = L"drone_C";
// struct_inventoryItem's members by their stable prefix (the GUID tail moves with a recook), and the two
// of its struct_betterSave `data` the storage lane reads.
inline constexpr const wchar_t* ItemPositionPrefix = L"position_";
inline constexpr const wchar_t* ItemRotationPrefix = L"rotation_";
inline constexpr const wchar_t* ItemAmountPrefix   = L"amount_";
inline constexpr const wchar_t* ItemDataPrefix     = L"data_";
inline constexpr const wchar_t* BetterSaveNamePrefix  = L"name_";
inline constexpr const wchar_t* BetterSaveClassPrefix = L"class_";
// The inventory screen (mainGamemode_C.ui_fli_inventory, a ui_freeLayoutInventory_C): whether it is
// open and shows a storage, the storage it shows beside the player's, its drag modifiers, the item
// widget a drag or a double-click names (a uicomp_FLI_item_C, whose ownerComponent and index name the
// row it draws), and the canvas verb that unbinds a canvas's item widgets, writing their places back
// into the rows (despawnCanvas), with the component a canvas is bound to.
inline constexpr const wchar_t* GamemodeInventoryScreenProp   = L"ui_fli_inventory";
inline constexpr const wchar_t* InventoryScreenClass          = L"ui_freeLayoutInventory_C";
inline constexpr const wchar_t* ScreenContainerComponentProp  = L"containerInventoryComponent";
inline constexpr const wchar_t* ScreenActiveProp              = L"inventoryActive";
inline constexpr const wchar_t* ScreenInContainerProp         = L"isInContainer";
inline constexpr const wchar_t* ScreenDragRmbProp             = L"dragRMB";
inline constexpr const wchar_t* ScreenStackCtrlProp           = L"canStack_ctrl";
inline constexpr const wchar_t* ScreenDoubleClickObjectProp   = L"doubleClickObject";
inline constexpr const wchar_t* ScreenBeginDragFn             = L"beginDrag";
inline constexpr const wchar_t* ScreenEndDragFn               = L"endDrag";
inline constexpr const wchar_t* ScreenDraggingProp            = L"currentlyDragging";
inline constexpr const wchar_t* ScreenPlayerCanvasProp        = L"flicanvas_player";
inline constexpr const wchar_t* ScreenHoldLmbProp             = L"holdButton_lmb";
inline constexpr const wchar_t* ScreenHoldRmbProp             = L"holdButton_rmb";
inline constexpr const wchar_t* ItemWidgetOwnerCanvasProp     = L"ownerCanvas";
inline constexpr const wchar_t* ItemWidgetOwnerComponentProp  = L"ownerComponent";
inline constexpr const wchar_t* ItemWidgetIndexProp           = L"index";
inline constexpr const wchar_t* CanvasDespawnFn               = L"despawnCanvas";
inline constexpr const wchar_t* CanvasConnectedComponentProp  = L"connectedComponent";
// The free-layout screen's placement members (storage_act's sort and settled-layout commits): a canvas's
// attached item widgets, an item widget's copy of its row (dataReference), its placement verbs (updateSlotPosition
// writes the widget's slot place and angle into that copy, setPosition sets truePosition and the slot, updateSlotInfo
// writes the copy into the component's row), and the screen's Shift-snap flag and grid size.
inline constexpr const wchar_t* CanvasItemsAttachedProp       = L"itemsAttached";
inline constexpr const wchar_t* ItemWidgetDataReferenceProp   = L"dataReference";
inline constexpr const wchar_t* ItemWidgetUpdateSlotPositionFn = L"updateSlotPosition";
inline constexpr const wchar_t* ItemWidgetSetPositionFn       = L"setPosition";
inline constexpr const wchar_t* ItemWidgetUpdateSlotInfoFn    = L"updateSlotInfo";
inline constexpr const wchar_t* ScreenSnapShiftProp           = L"snapToGrid_shift";
inline constexpr const wchar_t* ScreenGridSizeProp            = L"gridSize";
// The gamemode's own verb that opens the inventory screen onto a container prop (mainGamemode_C::openPropInv(prop)),
// the one the drone, the sack and the ATV hand their container to. A sleeper's loot opens through it.
inline constexpr const wchar_t* GamemodeOpenPropInvFn         = L"openPropInv";
inline constexpr const wchar_t* GamemodeOpenPropInvParam      = L"prop";
// prop_container_C's own verb for a person's interact: it opens the screen onto that container. The second way a
// sleeper's loot is opened when openPropInv leaves the screen closed.
inline constexpr const wchar_t* ContainerOpenFn               = L"openContainer";
// A sleeper (an offline player lying in the world, coop/player/sleepers) carries its inventory in a hidden container
// of this class, spawned on every peer and never saved, enrolled or drawn: a plain crate, whose begin-play only
// makes itself its storage's owner. Loaded by path when no instance has loaded the class yet.
inline constexpr const wchar_t* SleeperLootContainerClass     = L"prop_container_crate_C";
inline constexpr const wchar_t* SleeperLootContainerPath      = L"/Game/objects/prop_container_crate.prop_container_crate_C";
// A rest bed whose class name holds this is a sleeping bag (laid on the floor), any other a bed (laid on its surface): the
// word a log line names an online sleeper's surface by (coop/player/sleepers).
inline constexpr const char*    SleepingBagToken               = "sleepingbag";
// prop_C.ignoreSav: the prop's own "leave me out of the save" bool, which its ignoreSave answer ORs in.
inline constexpr const wchar_t* PropIgnoreSavProp             = L"ignoreSav";

// 0.10.0's player inventory (ue_wrap/actors/inventory_component): mainPlayer_C's own
// inventoryComponent_player, its three item arrays (the carried grid, the hands and hotbar, the
// equipment slots), the game's own byte codec of a whole object (the native ThingsAndStuffLib's
// ObjectToBytes and BytesIntoObject, which mainPlayer_C.betterGetData and betterLoadData call on the
// component), and updateEquipment, which rebuilds the equipped state from the equipment slots.
inline constexpr const wchar_t* MainPlayerInventoryComponent = L"inventoryComponent_player";
inline constexpr const wchar_t* InventoryCarriedArray        = L"canvasInventory";
inline constexpr const wchar_t* InventoryHandsArray          = L"SlotArray_A";
inline constexpr const wchar_t* InventoryEquipmentArray      = L"SlotArray_B";
inline constexpr const wchar_t* ThingsAndStuffLibClass       = L"ThingsAndStuffLib";
inline constexpr const wchar_t* ObjectToBytesFn              = L"ObjectToBytes";
inline constexpr const wchar_t* BytesIntoObjectFn            = L"BytesIntoObject";
inline constexpr const wchar_t* MainPlayerUpdateEquipmentFn  = L"updateEquipment";
// The item key of a prop: prop_C.name, the row name of the game's item tables (list_props,
// list_weapons) that prop_C::init was built from. On 0.10.0 the hand's actor is the only live trace of
// the hotbar's selected item that the reflected component exposes by name, so the key the hit judge
// looks swings up by is read off the held actor.
inline constexpr const wchar_t* PropItemKeyProp              = L"name";
inline constexpr const wchar_t* CreateSaveGameObjectFn       = L"CreateSaveGameObject";

// 0.10.0's one take predicate on a prop (the player-interface function the inventory's addItemAsObject and the hotbar
// route ask, with its `ignore` out param true = refused): heavy, static, frozen, a row that cannot be collected or held,
// or one that ignores interactions. It replaced prop_C's canBeCollected and canBePickedUp, which 0.10.0 no longer has.
// ignoreSave is int_save's answer, asked beside it (out param `ignoreSave`). The functions' own names are the literals
// of the prop_gather fact rows, which the fact-row gate reads from the source.
inline constexpr const wchar_t* PropRefusedOutParam             = L"ignore";
inline constexpr const wchar_t* PropIgnoreSaveOutParam          = L"ignoreSave";

// mainPlayer_C::addObjectToInventory(object, out success): the one take of a prop into an inventory in 0.10.0. Its
// callers (the R release, the hold timer) show the "Cannot collect" hint themselves when `success` stays false.
inline constexpr const wchar_t* AddObjectToInventoryObjectParam  = L"object";
inline constexpr const wchar_t* AddObjectToInventorySuccessParam = L"success";

// The static mesh prop_C::init sets when its props-table row gives no mesh (and no meshOverride).
inline constexpr const wchar_t* PropErrorMesh = L"00000000error";

// Actor spawning (the BlueprintCallable deferred-spawn pair the K2 SpawnActorFromClass node uses)
// and transform get/set.
inline constexpr const wchar_t* GameplayStaticsClass = L"GameplayStatics";

// UGameplayStatics::BeginDeferredActorSpawnFromClass is the deferred-spawn entry every BP "spawn
// actor of class" graph compiles to; intercepting it is one engine-level hook for every NPC and
// spawner class (the constant is BeginDeferredSpawnFn below).

// The NPC class allowlist. On a client every BeginDeferredActorSpawnFromClass for a class in this
// list is suppressed (the interceptor returns true with ActorClass zeroed, so the BP graph
// receives nullptr and bails); the host runs its spawners and the client's NPCs arrive through
// the EntityPose batch. All are targeting-compatible with the mainPlayer_C orphan puppet
// (AIPerception, PawnSensing or a Cast to mainPlayer_C reach a real Pawn of the same class).
inline constexpr const wchar_t* NpcClass_Zombie       = L"npc_zombie_C";
inline constexpr const wchar_t* NpcClass_KerfurOmega  = L"kerfurOmega_C";
inline constexpr const wchar_t* NpcClass_Krampus      = L"npc_krampus_C";
inline constexpr const wchar_t* NpcClass_Funguy       = L"npc_funguy_C";
inline constexpr const wchar_t* NpcClass_GoreSlither  = L"npc_goreSlither_C";
inline constexpr const wchar_t* NpcClass_Insomniac    = L"insomniac_C";
inline constexpr const wchar_t* NpcClass_Fossilhound  = L"fossilhound_C";
inline constexpr const wchar_t* NpcClass_Antibreather = L"antibreather_C";
inline constexpr const wchar_t* NpcClass_Orborb       = L"npc_orborb_C";
inline constexpr const wchar_t* NpcClass_ArirFollower = L"npc_arirFollower_C";
inline constexpr const wchar_t* NpcClass_AriralShooter   = L"npc_ariral_shooter_C";
inline constexpr const wchar_t* NpcClass_AriralPigBeater = L"npc_ariral_pigBeater_C";
// Event and late-game creatures in the same layer (verified against the CXX header dump):
// killerwisp_C (Akillerwisp_C : ACharacter, the lethal yellow Killer Wisp, not the base wisp_C
// swarm nor the coloured per-player wisps; AIPerception plus a Target APawn; spawned by
// ticker_yellowWispSpawner_C late in the game) and ventCrawler_C (AventCrawler_C : ACharacter, the
// ventCrawler event). Both ride the BeginDeferred interceptor and the pose stream; the vent
// crawler's wall crawl is a known yaw-only gap.
inline constexpr const wchar_t* NpcClass_KillerWisp   = L"killerwisp_C";
inline constexpr const wchar_t* NpcClass_VentCrawler  = L"ventCrawler_C";
// The other story-event creatures. Their packages are hard imports of the eventer and of the
// level-placed gray controller and balls follower, so they load with the level, as ventCrawler_C
// does. grayboar_C stays out: its SuperStruct is prop_C, not Character (the boar-war prop is the
// prop lane's); only the grayboarPawn_C helper it mints is a Character.
inline constexpr const wchar_t* NpcClass_GrayTest     = L"grayTest_C";      // graysforest pack
inline constexpr const wchar_t* NpcClass_Eg           = L"eg_C";            // eggvasion and night eggs
inline constexpr const wchar_t* NpcClass_TentacleBall = L"tentacleBall_C";  // tentacleBalls pack
// wisp_C (Awisp_C : ACharacter): the wispSwarm-event wisp and the ambient sky wisp
// (ticker_wispSpawner_C spawns at absolute map coordinates, world-anchored, so host-authoritative
// too). Both spawn paths are EX_CallMath, caught by the ufunction_hook Func thunk on
// BeginDeferred, source-gated by FFrame::Object's class. A mirror spawns invisible and fades in
// at the tick-driven landing edge, so it keeps its actor tick (CMC parked) and the pose lane
// drives the landing (ue_wrap::wisp::DriveWispLanding). The 8 colour variants are direct
// ACharacter subclasses, not wisp_C children, so each is listed.
inline constexpr const wchar_t* NpcClass_Wisp         = L"wisp_C";
inline constexpr const wchar_t* NpcClass_WispB        = L"wisp_b_C";
inline constexpr const wchar_t* NpcClass_WispRed      = L"wisp_red_C";
inline constexpr const wchar_t* NpcClass_WispBl       = L"wisp_bl_C";
inline constexpr const wchar_t* NpcClass_WispW        = L"wisp_w_C";
inline constexpr const wchar_t* NpcClass_WispG        = L"wisp_g_C";
inline constexpr const wchar_t* NpcClass_WispO        = L"wisp_o_C";
inline constexpr const wchar_t* NpcClass_WispP        = L"wisp_p_C";
inline constexpr const wchar_t* NpcClass_WispBlu      = L"wisp_blu_C";
// geomOcta_C (AgeomOcta_C : ACharacter): the Geom Octahedron. Host-authored; a client's mirror keeps its presentation
// and has only its decisions refused (coop/creatures/npc_mirror_brain).
inline constexpr const wchar_t* NpcClass_GeomOcta     = L"geomOcta_C";
// kavotia_C (a Character: its graph reads character.charactermovement and mesh): rozitalHole's patrollers, minted
// in kavotiaPatrolController_C's BeginPlay under EX_CallMath, three rings of them. A package hard-imported by that
// controller, itself an ObjectConst of event_bottomHoleController_C, an ObjectConst of the eventer's runEvent, so it
// loads with the level as the other story creatures do.
inline constexpr const wchar_t* NpcClass_Kavotia      = L"kavotia_C";
// figura_C (a Character on the kerfur-omega body AnimBP): the figure the gamemode's CustomEvent_3 drops 200 m from the
// roller's pawn, 50 m up, at a 1 % roll. Its only spawn site is that body, under EX_CallMath. It stands there turning
// its mesh toward the local camera from its tick, and its 0.1 s timer destroys it once the local camera is within 175
// units of it, so a mirror keeps its actor tick and has that timer refused (coop/world/spawn_authority).
inline constexpr const wchar_t* NpcClass_Figura       = L"figura_C";

// The allowlist array: resolved through R::FindClass and cached per entry. The classes load on the
// first gameplay-level transition, not at the menu: the install waits for the first one to load,
// then goes live, and any entry the level has not loaded yet binds lazily later, so one missing
// class never gates the rest. sizeof-derived, so a new entry binds everywhere.
inline constexpr const wchar_t* kNpcAllowlist[] = {
    NpcClass_Zombie,
    NpcClass_KerfurOmega,
    NpcClass_Krampus,
    NpcClass_Funguy,
    NpcClass_GoreSlither,
    NpcClass_Insomniac,
    NpcClass_Fossilhound,
    NpcClass_Antibreather,
    NpcClass_Orborb,
    NpcClass_ArirFollower,
    NpcClass_AriralShooter,
    NpcClass_AriralPigBeater,
    NpcClass_KillerWisp,   // yellow Killer Wisp (late-game; AIPerception like the 12)
    NpcClass_VentCrawler,  // the ventCrawler event (its wall crawl is a yaw-only gap)
    NpcClass_GrayTest,
    NpcClass_Eg,
    NpcClass_TentacleBall,
    NpcClass_Wisp,         // wispSwarm-event and ambient sky wisp (both host-authoritative)
    NpcClass_WispB,        // sky-wisp color variants (ticker_wispSpawner map; direct ACharacter
    NpcClass_WispRed,      //   subclasses, NOT wisp_C children -- each needs its own row)
    NpcClass_WispBl,
    NpcClass_WispW,
    NpcClass_WispG,
    NpcClass_WispO,
    NpcClass_WispP,
    NpcClass_WispBlu,
    NpcClass_GeomOcta,          // the Geom Octahedron; host-authored ACharacter, pose rides EntityPose
    NpcClass_Kavotia,           // rozitalHole's patrollers; host-authored ACharacter, brain refused on a mirror
    NpcClass_Figura,            // the gamemode's CustomEvent_3 figure; host-authored ACharacter, self-destroy timer refused on a mirror
};
inline constexpr size_t kNpcAllowlistSize = sizeof(kNpcAllowlist) / sizeof(kNpcAllowlist[0]);

// The non-Character event actors the Character-only NPC mirror cannot replicate: npc_pose_drive
// drives position, yaw and the CMC, so these AActor/APawn ships and saucers get a transform-only
// full-rotation mirror (coop::element::WorldActor, coop/world_actor_sync). A second BeginDeferred
// interceptor consumes this list, disjoint from kNpcAllowlist. Verified AActor/APawn in the CXX
// header dump; matched by name and kept curated.
inline constexpr const wchar_t* kWorldActorAllowlist[] = {
    L"ufoDropper_body_C",          // ufoDropper delivery saucer body (the gray "drop pod")
    L"ufoDropper_car_C",           // ufoDropper car variant
    L"ufoDropper_tank_C",          // ufoDropper tank variant (graystank with the eventer's `combat` set)
    L"ufoDropper_pig_C",           // ufoDropper pig variant (graystank otherwise)
    L"rozitBorg_C",                // Rozital mothership (borgRozital event)
    L"arirShip_C",                 // ariral ship
    L"skyUfo_C",                   // high-altitude sky UFO
    L"jellyfish_C",                // space jellyfish
    L"morningUfo_C",               // morningGay event UFO
    L"tentacleBallsFollower_C",    // tentacle-balls follower
    L"soltomiaCleaning_C",         // soltomia cleaning craft
    L"kocker_C",                   // kocker
    L"skyFallingEvent_C",          // "Sky Falling" event body (AskyFallingEvent_C; distinct from skyFallingEvent's emitter cue)
    L"superEgger_C",               // super egger
    L"igetis_C",                   // igetis
    L"firetank_C",                 // firetank (APawn -- transform-only mirror is layout-agnostic)
    L"piramidSubpawn_C",           // pyramid sub-pawn (APawn; the SANDBOX piramidTest_C child, not the event)
    L"piramid2_C",                 // walking pyramid event actor (a plain AActor; brain suppression and the
                                   // gather relay: coop/creatures/piramid_sync)
    L"baocoin_C",                  // the sell gun's coin: Abaocoin_C : AActor, not Aprop_C, so it rides no prop
                                   // lane; host-minted and mirrored (coop/items/coingun_sync). The mint is
                                   // EX_CallMath, so this entry alone does nothing: prop_coingun_C in
                                   // kExSpawnSourceClasses lets the Func-thunk drain reach
                                   // world_actor_sync::HostEnrollExSpawn, which allocates the eid.
    // The 0.10.0 UFO encounters' products. Each is minted inside its controller's ubergraph
    // (EX_CallMath), so these entries act only through the source rows in npc_world_enum; a
    // client's controllers and the children's verbs are refused in coop/world/spawn_authority.
    L"ufo_knob_C",                 // the knob flock (ufo_knob_controller_C::startEvent)
    L"ufo_UK_C",                   // the UK flock (ufo_UK_controller_C::beginEvent, ufoClass)
    L"ufo_UK_enemy_C",             // the enemy UK flock (the enemy controller's ufoClass)
    // The story-event scenes (coop/world/event_scene_sync): each minted under EX_CallMath, so these act only
    // through their source rows in npc_world_enum.
    L"obelisk_C",                  // the warning obelisk (trigger_spawnProp_C, the obelisk box's spawner)
    L"holeBottom_C",               // rozitalHole's room under the far hole (event_bottomHoleController_C)
    L"HoelUfo_C",                  // the super fog's UFO (superFog_C::spawnUfo; the source row in npc_world_enum,
                                   // the client's spawnUfo refused in coop/world/weather_fog)
    // The gamemode's ball UFO (mainGamemode_C::ufo_ball mints it above the host's camera; the source row in
    // npc_world_enum, the client's own roll refused in coop/world/spawn_authority). A transform alone cannot
    // mirror it: its `set` randomizes the heading and the endpoints, which the birth blob carries
    // (coop/world/ufo_ball_sync).
    L"ufo_ballfo_C",
    // The gamemode's other UFO rolls (ufo_boo, ufo_joel, ufo_pill), minted the same way and refused on a client the same
    // way; each carries its own setup or progress in the birth blob (coop/world/ufo_event_sync).
    L"ufo_boofo_spawn1_C",         // the boofo fly-by (a ufo_boofo_C child; ufo_boo)
    L"ufo_joel_C",                 // the radio tower's UFO (ufo_joel)
    L"ufo_pillfo_C",               // the lake glow's UFO (ufo_pill)
    // Not listed yet: a crashed ship's wreck (UKufoStuck_C) and crater (heavyDebrisCrater_C), whose
    // base classes are unverified -- a prop-derived wreck would ride the prop lane as well.
};
inline constexpr size_t kWorldActorAllowlistSize =
    sizeof(kWorldActorAllowlist) / sizeof(kWorldActorAllowlist[0]);

// The eventer's world-actor outputs (trigger_eventer_C::runEvent, each minted under EX_CallMath, so
// they cross only through the eventer's source row in npc_world_enum): ventKnocker's knocker (knocks
// on an overlap, then destroys itself), morningGay's UFO (shown and flown on a player's overlap; its
// tick moves its root), borgRozital's mothership (its tick flies the `ships` component across the sky
// over 480 s), and the fallbody / fallcar droppers (ufoDropper_C children: a player's overlap plays a
// timeline that drops a prop, and a 10 s timer kills a random kerfur, both refused on a client).
inline constexpr const wchar_t* KockerClass         = L"kocker_C";
inline constexpr const wchar_t* MorningUfoClass     = L"morningUfo_C";
inline constexpr const wchar_t* RozitBorgClass      = L"rozitBorg_C";
inline constexpr const wchar_t* UfoDropperBodyClass = L"ufoDropper_body_C";
inline constexpr const wchar_t* UfoDropperCarClass  = L"ufoDropper_car_C";
// graystank's droppers (runEvent('graystank'): the tank when the eventer's `combat` is set, else the pig).
// Both are ufoDropper_C children whose BeginPlay runs the parent's and then breaks generator 0: on a
// client mirror the parent's kerfur kill and drop are refused (spawn_authority) and the break is the
// power grid's to refuse (coop/world/power_grid), so a mirror is the saucer alone.
inline constexpr const wchar_t* UfoDropperTankClass = L"ufoDropper_tank_C";
inline constexpr const wchar_t* UfoDropperPigClass  = L"ufoDropper_pig_C";
// boarwar's controller (runEvent('boarwar') mints it above the treehouse): its ubergraph throws grayboar_C
// props (the prop lane carries them), mints two npc_ariral_shooter_C 60 s and 120 s in, and four firetank_C
// once the boars are spent, all under EX_CallMath, so its creatures cross only through its source row in
// npc_world_enum. The firetanks stay out of that row: a firetank_C mirror's BeginPlay arms five AI timers
// (update, senseupdate, fire, speak, makehealthnoise) that no row refuses yet.
inline constexpr const wchar_t* BoarInvasionClass = L"boarInvasion_C";
// ---- The story-event scenes (coop/world/event_scene_sync, ue_wrap/actors/event_scenes) ----
// Warning Obelisk: the obelisk box's trigger_spawnProp_C (its runTrigger mints the class its prop row names) mints
// obelisk_C, an actor_save_C. Its BeginPlay rises the mesh, then plays the fall timeline `a`; the finish
// (A__FinishedFunc) is all gameplay but its landing presentation: the radial impulse, seventy rock props, the
// prop_obelisk_C it leaves, getMainPlayer's punch and `fallen` ten seconds on. `obelisk_land` is the landing's audio,
// StaticMesh the falling body, ParticleSystem the dust.
inline constexpr const wchar_t* TriggerSpawnPropClass   = L"trigger_spawnProp_C";
inline constexpr const wchar_t* ObeliskClass            = L"obelisk_C";
inline constexpr const wchar_t* ObeliskFallenProp       = L"fallen";
inline constexpr const wchar_t* ObeliskLandAudioProp    = L"obelisk_land";
inline constexpr const wchar_t* ObeliskMeshProp         = L"StaticMesh";
inline constexpr const wchar_t* ObeliskDustProp         = L"ParticleSystem";
inline constexpr const wchar_t* ObeliskLandShakeClass   = L"obeliskLandShake_C";
// lib_C::setEvent(isEventActive, deactivateAmbientTrack, __WorldContext): the running-event refcount's verb (the
// obelisk's begin-play adds one its landing would take back).
inline constexpr const wchar_t* LibClass                = L"lib_C";
inline constexpr const wchar_t* LibSetEventFn           = L"setEvent";
inline constexpr const wchar_t* LibSetEventActiveParam  = L"isEventActive";
inline constexpr const wchar_t* LibWorldContextParam    = L"__WorldContext";
// Garage Soltomia: the eventer mints soltomiaCleaning_C hidden. A player's overlap of its box shows it (the
// squeak, the particles, `active`, the talking timer) and moves the ATV and jams three doors; the garage door's
// dooropened runs `opened`, which plays the montage on `solt`; the montage's `throw` notify mints the sponge.
inline constexpr const wchar_t* SoltomiaCleaningClass   = L"soltomiaCleaning_C";
inline constexpr const wchar_t* SoltomiaOpenedFn        = L"opened";
inline constexpr const wchar_t* SoltomiaTalkingFn       = L"talking";
inline constexpr const wchar_t* SoltomiaActiveProp      = L"active";
inline constexpr const wchar_t* SoltomiaSqueakProp      = L"squeak";
inline constexpr const wchar_t* SoltomiaDustProp        = L"ParticleSystem";
inline constexpr const wchar_t* SoltomiaSpongeMeshProp  = L"StaticMesh";
inline constexpr const wchar_t* SoltomiaThrowNotify     = L"throw";
inline constexpr const wchar_t* SoltomiaNotifyNameParam = L"NotifyName";
// Hole Repair: the eventer mints event_bottomHoleController_C (an actor_save_C); its box's overlap mints
// holeBottom_C (the room), holeDevice_C and kavotiaPatrolController_C, whose BeginPlay mints the kavotia_C rings
// and an igetis_C at the device.
inline constexpr const wchar_t* BottomHoleControllerClass = L"event_bottomHoleController_C";
inline constexpr const wchar_t* HoleBottomClass           = L"holeBottom_C";
inline constexpr const wchar_t* KavotiaPatrolControllerClass = L"kavotiaPatrolController_C";
inline constexpr const wchar_t* IgetisClass               = L"igetis_C";
// Gray Invasion's level UFO shader (getActorOfClass from boarInvasion_C's BeginPlay): setActive(bEnabled) shows or
// hides it and its audio, flyaway() plays its exit timeline, which ends in setActive(false).
inline constexpr const wchar_t* UfoShieldShaderClass    = L"ufoShieldShader_C";
inline constexpr const wchar_t* UfoShieldSetActiveFn    = L"setActive";
inline constexpr const wchar_t* UfoShieldEnabledParam   = L"bEnabled";
inline constexpr const wchar_t* UfoShieldFlyAwayFn      = L"flyaway";
inline constexpr const wchar_t* FiretankClass           = L"firetank_C";
// The anti-gravity prank: the level-placed trigger_agrav_C. runTrigger waits for a pawn in its `bounds`, breaks a
// generator, cloak(true) (its shimmer timeline), gathers the props within 10000 of player 0's camera (gaher), turns
// their gravity off one by one, waits 5-7 s, turns it back on and cloak(false). `arirhover_cue` is its hover sound.
inline constexpr const wchar_t* TriggerAgravClass       = L"trigger_agrav_C";
inline constexpr const wchar_t* AgravCloakFn            = L"cloak";
inline constexpr const wchar_t* AgravForwardParam       = L"forward";
inline constexpr const wchar_t* AgravHoverProp          = L"arirhover_cue";
inline constexpr float          kAgravGatherRadiusCm    = 10000.f;  // gaher's SphereOverlapActors radius
// UPrimitiveComponent's gravity verbs (engine_physics).
inline constexpr const wchar_t* SetEnableGravityFn      = L"SetEnableGravity";
inline constexpr const wchar_t* IsGravityEnabledFn      = L"IsGravityEnabled";
// The vent crawler (ventCrawler_C): its `Box`'s begin-overlap starts the crawl (a DoOnce with no actor filter:
// two vent footsteps, then moveTL, the crawl, whose StepSounds and BreakVent events play its footsteps and the
// vent's break at its place, as GameplayStatics one-shots).
inline constexpr const wchar_t* VentCrawlerBoxProp      = L"Box";
inline constexpr const wchar_t* VentCrawlerBoxOverlapFn =
    L"BndEvt__ventCrawler_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature";
inline constexpr const wchar_t* BoxGetScaledExtentFn    = L"GetScaledBoxExtent";
// The host event actors whose positional one-shots (PlaySoundAtLocation, SpawnEmitterAtLocation) the sound relay
// carries to every client (coop/world/event_sound_relay). Every entry's client copy must have the bodies that play
// them refused, so a client never plays its own beside the host's. The crawler's bodies are refused in
// spawn_authority; the knocker's knock sequence starts at its box overlap, refused there too (kocker_C.knock is a
// PlaySoundAtLocation at its own place, metaldoorknock_cue; its seven knocks are the host's alone).
inline constexpr const wchar_t* kPositionalCueSourceClasses[] = {
    NpcClass_VentCrawler,
    KockerClass,
};
// The event actors a client's player starts by standing in their `Box` (a begin-overlap with no actor filter, under a
// DoOnce). A client's puppet never overlaps it on the host, so the host tests the puppets against the box itself
// (coop/world/event_scene_crawler). Both classes' overlap handler is the ventCrawler's copy, so one name serves.
inline constexpr const wchar_t* kBoxStartedSceneClasses[] = {
    NpcClass_VentCrawler,
    KockerClass,
};
inline constexpr const wchar_t* SpawnEmitterAtLocationFn = L"SpawnEmitterAtLocation";
inline constexpr const wchar_t* ParticleTemplateProp     = L"Template";

// World-actor classes the joiner's save carries (the two actor_save_C children, and firetank_C, which implements the
// save interface itself -- docs/join.md): the connect snapshot leaves them out, so a joiner keeps the copy its own
// load made instead of a second, mirrored one.
inline constexpr const wchar_t* kWorldActorSaveCarriedClasses[] = {
    ObeliskClass,
    HoleBottomClass,
    FiretankClass,
};

// The arirFollower event's spawner: runTrigger mints npc_arirFollower_C at its own transform
// (EX_CallMath), so the npc lane sees it only through a source row.
inline constexpr const wchar_t* TriggerSpawnFollowingArirClass = L"trigger_spawnFollowingArir_C";

// ufo_ballfo_C (0.10.0 bytecode capture): the gamemode's ball UFO. Its components: `center` (turned to a random yaw
// by `set`), `a` and `b` (the endpoints, placed relative to `center` by `set`: x = -/+size and a random y), `balls`
// (the visible ball; the tick slides it toward `b` at `speed` and the actor destroys itself on arrival) and `lift`
// (the descent, derived from the ball's progress). Only the five values `set` randomizes plus the ball's current
// place need to cross; the rest is the class's own deterministic tick.
inline constexpr const wchar_t* UfoBallClass           = L"ufo_ballfo_C";
inline constexpr const wchar_t* UfoBallCenterProp      = L"center";
inline constexpr const wchar_t* UfoBallAProp           = L"a";
inline constexpr const wchar_t* UfoBallBProp           = L"b";
inline constexpr const wchar_t* UfoBallBallsProp       = L"balls";

// The gamemode's other UFO rolls (0.10.0 bytecode capture). Each timer body rolls, mints its actor under EX_CallMath and
// re-arms itself: ufo_boo (the boofo fly-by over the map, a night roll), ufo_joel (the radio tower's UFO, rolled while a
// player stands at the tower's top), ufo_pill (the lake glow's UFO), and ufo_midas, which mints nothing: it runs the
// level-placed midasufotest_C through its trigger interface.
//   ufo_boofo_c: gen() (the construction script's and BeginPlay's) lays `flyspline` as splinepoints+1 points, each the
//   `travelspline` point at i/splinepoints pushed out by `radius` at a random angle about the spline's forward axis and
//   scaled by `squish`; the tick advances `alpha` along it (length = the flyspline's length) and destroys the actor at
//   the end. Its tick is enabled by BeginPlay five seconds in.
//   ufo_joel_c, ufo_pillfo_c: no randomness; a timeline each (joel's `moveufo`; pill's `a`, whose finish plays
//   `timeline_0`, whose finish destroys the actor) drives the flight.
inline constexpr const wchar_t* UfoBoofoClass          = L"ufo_boofo_spawn1_C";
inline constexpr const wchar_t* UfoBoofoTravelSplineProp = L"travelspline";
inline constexpr const wchar_t* UfoBoofoFlySplineProp  = L"flyspline";
inline constexpr const wchar_t* UfoBoofoSplinePointsProp = L"splinepoints";
inline constexpr const wchar_t* UfoBoofoRadiusProp     = L"radius";
inline constexpr const wchar_t* UfoBoofoSquishProp     = L"squish";
inline constexpr const wchar_t* UfoBoofoAlphaProp      = L"alpha";
inline constexpr const wchar_t* UfoBoofoLengthProp     = L"length";
inline constexpr const wchar_t* UfoJoelClass           = L"ufo_joel_C";
inline constexpr const wchar_t* UfoJoelMoveTimelineProp = L"moveufo";
inline constexpr const wchar_t* UfoPillfoClass         = L"ufo_pillfo_C";
inline constexpr const wchar_t* UfoPillfoTimelineAProp = L"a";
inline constexpr const wchar_t* UfoPillfoTimeline0Prop = L"timeline_0";
// USplineComponent verbs the boofo's course is read and laid with.
inline constexpr const wchar_t* SplineLocationAtPointFn = L"GetLocationAtSplinePoint";
inline constexpr const wchar_t* SplineLocationAtTimeFn  = L"GetLocationAtTime";
inline constexpr const wchar_t* SplineRightAtTimeFn     = L"GetRightVectorAtTime";
inline constexpr const wchar_t* SplineUpAtTimeFn        = L"GetUpVectorAtTime";
inline constexpr const wchar_t* SplineClearPointsFn     = L"ClearSplinePoints";
inline constexpr const wchar_t* SplineAddPointFn        = L"AddSplinePoint";
inline constexpr const wchar_t* SplineUpdateFn          = L"UpdateSpline";
inline constexpr const wchar_t* SplineLengthFn          = L"GetSplineLength";
// midasufotest_C: the level-placed UFO ufo_midas switches on (runTrigger -> setActive(true)); its tick advances `a`
// along `spline` and switches it off at 1.
inline constexpr const wchar_t* MidasUfoClass          = L"midasufotest_C";
inline constexpr const wchar_t* MidasUfoSetActiveFn    = L"setActive";
inline constexpr const wchar_t* MidasUfoActiveParam    = L"active";
inline constexpr const wchar_t* MidasUfoProgressProp   = L"a";

// World-actor mirrors whose whole presentation is their own deterministic tick on a component, not
// the root the pose drive moves: the client mirror keeps its actor tick (world_actor_mirror). Every
// entry's tick must write no shared state.
inline constexpr const wchar_t* kWorldActorTickDrivenClasses[] = {
    RozitBorgClass,  // the tick lerps `ships` by its alpha, shakes the local camera, destroys itself at alpha 1
    UfoJoelClass,    // the tick spins its components and eases its look; its moveufo timeline flies it
    UfoPillfoClass,  // the tick spins its saucer; its two timelines fly and end it
    UfoBallClass,    // the tick spins and slides `balls` from a to b at a constant speed and destroys the actor on
                     // arrival; the birth blob seeds its endpoints and position (coop/world/ufo_ball_sync)
};

// The transient physics props the host's ambient spawners materialise through
// BeginDeferredActorSpawnFromClass whose own Init is BP-internal (EX_LocalVirtualFunction, past
// the ProcessEvent detour). The spawn call is observable, so coop/host_spawn_watcher catches it at
// the BeginDeferred POST seam, mirrors the prop host-to-client through the PropSpawn pipeline
// (keyless, eid-routed) and death-watches its SetLifeSpan(600) expiry. Exact-pointer matched leaf
// classes. Local fall physics diverges per peer by design.
inline constexpr const wchar_t* kAmbientPropSpawnMirrorClasses[] = {
    L"prop_food_pinecone_C",  // the scare: spawns high, drops, bounces, rolls / lies
    L"prop_stick_C",          // pineconeSpawner forage branch
    L"prop_crystal_C",        // pineconeSpawner forage branch
};
inline constexpr size_t kAmbientPropSpawnMirrorClassesSize =
    sizeof(kAmbientPropSpawnMirrorClasses) / sizeof(kAmbientPropSpawnMirrorClasses[0]);

inline constexpr const wchar_t* BeginDeferredSpawnFn = L"BeginDeferredActorSpawnFromClass";
inline constexpr const wchar_t* FinishSpawningActorFn = L"FinishSpawningActor";

// The story-save load: the game's own entry (GameplayStatics::LoadGameFromSlot,
// mainGameInstance_C::setSaveSlotObject with loadObjects=true, then untitled_1; the save selects
// the mode).
inline constexpr const wchar_t* LoadGameFromSlotFn = L"LoadGameFromSlot";
inline constexpr const wchar_t* SetSaveSlotObjectFn = L"setSaveSlotObject";

// 0.10.0's world folders (ue_wrap/engine/world_folder): SaveGames/<world>/ holds main and autosave
// (.sav + .meta + .png) and persistent.sav, all written through the CompressedSaveLoadLib plugin.
// LoadGameCompressed(name, path, ext, userIndex) -> USaveGame*;
// SaveGameCompressed(object, path, name, ext, userIndex) -> bool. Positional: the plugin's
// parameter names are read off the UFunction at the call.
inline constexpr const wchar_t* CompressedSaveLibClass   = L"CompressedSaveLoadLib";
inline constexpr const wchar_t* LoadGameCompressedFn     = L"LoadGameCompressed";
inline constexpr const wchar_t* SaveGameCompressedFn     = L"SaveGameCompressed";
inline constexpr const wchar_t* WorldEntryMain           = L"main";
inline constexpr const wchar_t* WorldEntryAutosave       = L"autosave";
inline constexpr const wchar_t* WorldPersistentName      = L"persistent";
inline constexpr const wchar_t* WorldSaveExt             = L".sav";
inline constexpr const wchar_t* WorldMetaExt             = L".meta";
// lib_C::createSaveSlotTreeDirectory(slotName) -> return, metadata_main, metadata_autosave,
// saveSlot_main, saveSlot_autosave, saveSlot_persistent: the game's own world creation.
inline constexpr const wchar_t* CreateSaveSlotTreeFn     = L"createSaveSlotTreeDirectory";
inline constexpr const wchar_t* SaveMetadataClass        = L"save_metadata_C";
inline constexpr const wchar_t* SaveMetadataRulesProp    = L"Rules";
inline constexpr const wchar_t* SaveMetadataGameModeProp = L"gamemode_type";
inline constexpr const wchar_t* SaveMetadataSavedTimeProp = L"savedtime";
inline constexpr const wchar_t* SaveMetadataAreaProp     = L"area";
// The level a world starts in (FName; None on a world made before 0.10.0 named one, which the
// game's launch reads as untitled_1) and the level its last save was in.
inline constexpr const wchar_t* SaveMetadataStarterMapProp   = L"starterMap";
inline constexpr const wchar_t* SaveMetadataCurrentLevelProp = L"currentLevel";
// The game instance's level state, both FNames. lib_C::loadLevel(level, option) writes
// currentLevel = level on its way into a gameplay level (None on its way to the menu), and
// mainGamemode_C::loadObjects skips the whole restore of a save -- player transform, every saved
// object, the player's own data, the triggers -- while it is None ("Cannot load objects: current
// level none"). The save menu's launch sets baseLevel = the metadata's starterMap first.
inline constexpr const wchar_t* GameInstanceCurrentLevelProp = L"currentLevel";
inline constexpr const wchar_t* GameInstanceBaseLevelProp    = L"baseLevel";
// The save's restore on a level's BeginPlay (mainGamemode_C::loadObjects) and the player's own
// part of it (mainPlayer_C::betterLoadData: pawn data, inventory, equipment).
inline constexpr const wchar_t* MainGamemodeLoadObjectsFn = L"loadObjects";
inline constexpr const wchar_t* MainPlayerBetterLoadDataFn = L"betterLoadData";
// saveSlot_C's own folder (stamped at creation, absolute) and entry name; saveToSlot writes
// <savepath>/<slotName>.sav through SaveGameCompressed.
inline constexpr const wchar_t* SaveSlotSavePathProp = L"savepath";
inline constexpr const wchar_t* SaveSlotSlotNameProp = L"slotName";
inline constexpr const wchar_t* SaveSelfFn               = L"saveSelf";  // saveSlot_C, save_metadata_C, save_persistent_C
// The world-save verbs a client must never run (coop/save/save_block). mainGamemode_C::save(asBackup,
// bypassEvent, asAutosave) is the pause menu's and the quicksave key's entry; saveSlot_C::save(asBackup,
// asAutosave, skipEventBlock) gathers then calls saveToSlot(asBackup, asAutosave), which writes the
// entry, the screenshot, the persistent object and the metadata; saveManual(folderSlot) and
// savePlayerOnly() call saveToSlot directly; save_metadata_C::saveMetadata(path, slot, bool) and the
// three classes' saveSelf write their own file.
inline constexpr const wchar_t* MainGamemodeSaveFn       = L"save";
inline constexpr const wchar_t* SaveSlotSaveFn           = L"save";
inline constexpr const wchar_t* SaveSlotSaveToSlotFn     = L"saveToSlot";
inline constexpr const wchar_t* SaveSlotSaveManualFn     = L"saveManual";
inline constexpr const wchar_t* SaveSlotSavePlayerOnlyFn = L"savePlayerOnly";
inline constexpr const wchar_t* SaveMetadataSaveFn       = L"saveMetadata";
inline constexpr const wchar_t* SavePersistentClass      = L"save_persistent_C";
inline constexpr const wchar_t* MainGamemodeSaveAutosaveParam = L"asAutosave";
// The quicksave key's input event on mainPlayer_C, which calls mainGamemode.save (the save test drives it).
inline constexpr const wchar_t* MainPlayerQuickSaveFn = L"InpActEvt_quicksave_K2Node_InputActionEvent_50";
inline constexpr const wchar_t* GameInstanceGameRulesProp = L"gameRules";
inline constexpr const wchar_t* SaveSlotLocalRulesProp   = L"localGameRules";
// The game's save menu (ui_saveSlots_C, a child of the main menu's switcher) and the parts of it the
// hosting window drives to create a world with the game's own rules editor.
inline constexpr const wchar_t* SaveSlotsWidgetClass     = L"ui_saveSlots_C";
inline constexpr const wchar_t* SaveSlotsOpenCreateFn    =
    L"BndEvt__button_create_K2Node_ComponentBoundEvent_8_OnButtonClickedEvent__DelegateSignature";
inline constexpr const wchar_t* SaveSlotsSubmitCreateFn  =
    L"BndEvt__umg_saveSlots_button_create_1_K2Node_ComponentBoundEvent_11_OnButtonClickedEvent__DelegateSignature";
inline constexpr const wchar_t* SaveSlotsInnerSwitcher   = L"WidgetSwitcher_308";

// The live host-world capture (save_capture.cpp): repopulate the world save from live actors, then
// serialise it to a scratch slot for a joiner (a kerfur the host turned on after its last
// autosave is captured live). All on AmainGamemode_C except SaveGameToSlot.
inline constexpr const wchar_t* MainGamemodeSaveObjectsFn  = L"saveObjects";   // saveObjects(bool quicksave[, bool skipEventBlock])
// 0.10.0's second saveObjects parameter: true gathers without asking lib_C::getEvent.
inline constexpr const wchar_t* MainGamemodeSaveObjectsSkipEventParam = L"skipEventBlock";
inline constexpr const wchar_t* MainGamemodeSaveTriggersFn = L"saveTriggers";  // saveTriggers()
inline constexpr const wchar_t* SaveGameToSlotFn           = L"SaveGameToSlot";  // (USaveGame*, FString slot, int32 idx) -> bool
inline constexpr const wchar_t* ActorClassName = L"Actor";  // owns K2_Get/SetActorLocation
inline constexpr const wchar_t* GetActorLocationFn = L"K2_GetActorLocation";
inline constexpr const wchar_t* GetActorRotationFn = L"K2_GetActorRotation";
inline constexpr const wchar_t* GetActorVelocityFn = L"GetVelocity";  // AActor::GetVelocity -> FVector (cm/s)
inline constexpr const wchar_t* GetActorScale3DFn = L"GetActorScale3D";  // AActor -> FVector
inline constexpr const wchar_t* CollectGarbageFn = L"CollectGarbage";  // KismetSystemLibrary static; schedules a full GC purge end-of-frame (post-sweep plateau collapse). Class constant @ the existing KismetSystemLibraryClass above.
inline constexpr const wchar_t* PropInventoryContainerPlayerClass = L"prop_inventoryContainer_player_C";  // PER-PLAYER inventory container -- never snapshot-expressed/broadcast/swept (prop_lifecycle::IsPerPlayerPropClass)
inline constexpr const wchar_t* GetActorForwardVectorFn = L"GetActorForwardVector";
inline constexpr const wchar_t* GetActorUpVectorFn = L"GetActorUpVector";
inline constexpr const wchar_t* SetActorRotationFn = L"K2_SetActorRotation";
inline constexpr const wchar_t* SetActorScale3DFn = L"SetActorScale3D";  // AActor::SetActorScale3D(FVector) -> void
inline constexpr const wchar_t* SetLifeSpanFn = L"SetLifeSpan";  // AActor::SetLifeSpan(float InLifespan) -> void
inline constexpr const wchar_t* SetActorTickEnabledFn = L"SetActorTickEnabled";
inline constexpr const wchar_t* SetActorHiddenInGameFn = L"SetActorHiddenInGame";        // AActor::SetActorHiddenInGame(bool bNewHidden) -- visual-only (instant-world deferred-hide)
inline constexpr const wchar_t* SetActorEnableCollisionFn = L"SetActorEnableCollision";  // AActor::SetActorEnableCollision(bool bNewActorEnableCollision) -- the Off mode of a mirror hide (a parked NPC, a held mirror)
inline constexpr const wchar_t* ActorHiddenProp = L"bHidden";  // AActor::bHidden, the bool SetActorHiddenInGame writes (read by the vent-cover census)
inline constexpr const wchar_t* DestroyActorFn = L"K2_DestroyActor";
inline constexpr const wchar_t* TeleportToFn = L"K2_TeleportTo";  // bool(FVector, FRotator); large-distance teleport that survives Character/CMC constraints
inline constexpr const wchar_t* GetComponentBoundsFn = L"GetComponentBounds";  // KismetSystemLibrary static (Component, out Origin, out BoxExtent, out SphereRadius)
inline constexpr const wchar_t* GetActorBoundsFn = L"GetActorBounds";   // (bool,FVector&,FVector&,bool); world-space AABB of the actor's mesh

// Controller removal: an auto-possessing remote pawn's second PlayerController fights the local
// player's input and view; GetController, Detach, destroy.
inline constexpr const wchar_t* PawnClassName = L"Pawn";
inline constexpr const wchar_t* GetControllerFn = L"GetController";
inline constexpr const wchar_t* DetachFromControllerFn = L"DetachFromControllerPendingDestroy";
// An AIController poses the body through the possession path with no viewport, input or camera.
inline constexpr const wchar_t* SpawnDefaultControllerFn = L"SpawnDefaultController";

// SetSkeletalMesh is owned by USkinnedMeshComponent and SetAnimClass by USkeletalMeshComponent;
// FindFunction does not climb to super, so each resolves from its own class.
inline constexpr const wchar_t* SkeletalMeshComponentClass = L"SkeletalMeshComponent";
inline constexpr const wchar_t* SkinnedMeshComponentClass = L"SkinnedMeshComponent";
inline constexpr const wchar_t* SetSkeletalMeshFn = L"SetSkeletalMesh";   // USkinnedMeshComponent
inline constexpr const wchar_t* SetAnimClassFn = L"SetAnimClass";         // USkeletalMeshComponent

// UStaticMeshComponent::SetStaticMesh: the trash-proxy mirror's mesh swap (pile <-> clump, the
// mesh resolved from chipType through getChipPileType); resolved from its own class.
inline constexpr const wchar_t* StaticMeshComponentClass = L"StaticMeshComponent";
inline constexpr const wchar_t* SetStaticMeshFn = L"SetStaticMesh";       // UStaticMeshComponent

// The broom (ue_wrap/actors/broom). Its stroke's chip-pile loop keeps the pile it is turning into a
// clump in an ubergraph local, the one both the clump's spawn and the pile's destroy read. The
// stroke is entered by the notify of the swing montage the right mouse button plays; the notify
// function's name carries its graph node's GUID, so a recook of the broom renames it. `player` is
// the holder the broom's graph writes on the button's press.
inline constexpr const wchar_t* BroomClass = L"prop_broom_C";
inline constexpr const wchar_t* BroomUbergraphFn = L"ExecuteUbergraph_prop_broom";
inline constexpr const wchar_t* BroomSweptPileLocal = L"K2Node_DynamicCast_AsActor_Chip_Pile";
inline constexpr const wchar_t* BroomStrokeNotifyFn = L"OnNotifyBegin_6E812E554132A6D770F1F3A21FF9EAC0";
inline constexpr const wchar_t* BroomNotifyNameParam = L"NotifyName";
inline constexpr const wchar_t* BroomStrokeNotifyName = L"clean";
inline constexpr const wchar_t* BroomHolderProp = L"player";

// The vacuum and the dispenser pile's vacuum verb (0.10.0 cook, read from the name tables of
// /Game/objects/prop_vacuum2 and /Game/objects/trashBitsPile). The vacuum's gather timer traces from
// its holder's arm, overlaps a sphere at the hit and, for a dispenser pile it lands on, casts to
// trashBitsPile_C and calls `vacuumed`, a custom event whose one parameter is `location` (the
// event's K2Node_CustomEvent_location local); the body pops one item. The vacuum's class is
// prop_vacuum2_C, and the table also names a prop_vacuum_C, so a vacuum is matched by the prefix.
inline constexpr const wchar_t* VacuumClassPrefix = L"prop_vacuum";
inline constexpr const wchar_t* TrashPileVacuumedFn = L"vacuumed";
inline constexpr const wchar_t* TrashPileVerbLocationParam = L"location";

// The breakable props whose own break spawns an explosion_C (ue_wrap/actors/breakable_explosive): the vending
// machine's `broken` spills half its stock, blasts at its `audio_sell` and ignites the spill (the live machine is the
// subclass prop_vending_soda_C), a gas can's `broken` and `broken_fire`, the three bombs, the explosive crystal and
// the explosive drive. Each is matched with its subclasses.
inline constexpr const wchar_t* BreakableExplosiveClasses[] = {
    L"prop_vending_C",   L"prop_gascan_C",          L"prop_bombBarrel_C",       L"prop_bombBottle_C",
    L"prop_bombBox_C",   L"prop_crystal_explode_C", L"prop_drive_explosive_C",
};

// The office cubicle partitions of the base's main room (ue_wrap/actors/cubicle): the class prop_cubicle_C (a prop_C
// whose begin-play can break it at once through a huge addDamage when `instaBreak` is set), and plain prop_C
// partitions whose list_props row is cubicleP_<n> (cubicleP_1 measured in a run's log). An FName keeps a trailing
// _<n> as its number, so the row base is matched by name index, whatever the number.
inline constexpr const wchar_t* CubicleClass = L"prop_cubicle_C";
inline constexpr const wchar_t* CubicleItemRowBase = L"cubicleP";

// The fire extinguisher and its wall mount (ue_wrap/actors/fire_extinguisher). The extinguisher's
// own members are its charge, its hand spray and its runaway thrust; the mount's `fireExt` names
// the extinguisher its box last snapped onto its attach point.
inline constexpr const wchar_t* FireExtinguisherClass = L"prop_fireExt_C";
inline constexpr const wchar_t* FireExtinguisherMountClass = L"fireExtHolder_C";
inline constexpr const wchar_t* FireExtMountHeldProp = L"fireExt";
inline constexpr const wchar_t* FireExtChargeProp = L"life";
inline constexpr const wchar_t* FireExtSprayingProp = L"firing";
inline constexpr const wchar_t* FireExtThrustingProp = L"active";
// The burst: shoot() fires the thrust (the thruster, its flame particles and its roar start, `active` is set and
// a latent loop drains `life`), and the three components it starts. Deactivate is the engine's own verb on each.
inline constexpr const wchar_t* FireExtShootFn = L"shoot";
inline constexpr const wchar_t* FireExtThrusterComp = L"PhysicsThruster";
inline constexpr const wchar_t* FireExtThrustFxComp = L"eff_fireExtThr";
inline constexpr const wchar_t* FireExtThrustSoundComp = L"fireExtThrust";
inline constexpr const wchar_t* ComponentDeactivateFn = L"Deactivate";
// Where prop_fireExt_C::getData files its own two members in the save record, read from the pak: the leaf class
// appends one group of each kind after the base prop's, `life` as floats[1][0] and `active` as bools[1][0].
inline constexpr int FireExtRecordGroup = 1;

// The held item's right mouse button, int_objects_C's playerHandUse_RMB / playerHandRelease_RMB:
// the player's input reaches the item in its hand through these, with itself as the argument.
inline constexpr const wchar_t* HandUseRmbFn = L"playerHandUse_RMB";
inline constexpr const wchar_t* HandReleaseRmbFn = L"playerHandRelease_RMB";
inline constexpr const wchar_t* HandUsePlayerParam = L"player";

// AmainPlayer_C::arm(customLength, out start, out end, out rotation): the reach segment every
// held-tool trace aims along, from the camera of player index 0.
inline constexpr const wchar_t* MainPlayerArmFn = L"arm";
inline constexpr const wchar_t* MainPlayerArmStartParam = L"start";
inline constexpr const wchar_t* MainPlayerArmEndParam = L"end";

// The kerfur AnimBP generated class (the asset AnimBlueprint_kerfurOmega_regular plus the '_C'
// suffix of BP-generated classes).
inline constexpr const wchar_t* AnimBPKerfurRegularClass = L"AnimBlueprint_kerfurOmega_regular_C";

// The dev freecam: a spawned ACameraActor the view is pointed at (SetViewTargetWithBlend), look
// from the game's own control rotation, moved by WASD per frame.
inline constexpr const wchar_t* CameraActorClass = L"CameraActor";
inline constexpr const wchar_t* ControllerClassName = L"Controller";
inline constexpr const wchar_t* GetControlRotationFn = L"GetControlRotation";
// The engine's own setter, the one the game's blueprints call (mainPlayer's bytecode:
// EX_VirtualFunction SetControlRotation). It carries no K2_ prefix: under that name it never
// resolved, and every caller took the direct field write.
inline constexpr const wchar_t* SetControlRotationFn = L"SetControlRotation";
inline constexpr const wchar_t* PlayerControllerClassName = L"PlayerController";
// APlayerController::ProjectWorldLocationToScreen(FVector, FVector2D&, bool
// bPlayerViewportRelative) -> bool: the world-to-viewport projection of the screen-space nameplates
// (false behind the camera).
inline constexpr const wchar_t* ProjectWorldToScreenFn = L"ProjectWorldLocationToScreen";
inline constexpr const wchar_t* SetViewTargetWithBlendFn = L"SetViewTargetWithBlend";
inline constexpr const wchar_t* PlayerCameraManagerClass = L"PlayerCameraManager";
inline constexpr const wchar_t* GetCameraLocationFn = L"GetCameraLocation";
inline constexpr const wchar_t* GetCameraRotationFn = L"GetCameraRotation";

// Component visibility (USceneComponent BlueprintCallable): forces the third-person body meshes
// visible on an unpossessed remote pawn.
inline constexpr const wchar_t* SceneComponentClass = L"SceneComponent";
inline constexpr const wchar_t* SetVisibilityFn = L"SetVisibility";
// K2_SetRelativeRotation runs the transform propagation (UpdateComponentToWorld and the child
// re-evaluation) that a direct RelativeRotation write skips.
inline constexpr const wchar_t* SetRelativeRotationFn = L"K2_SetRelativeRotation";
// K2_SetRelativeLocation(NewLocation, bSweep, SweepHitResult, bTeleport): the same propagation for a component's
// relative location. door_C's leaf roots (door_r_root, door_l_root) are placed by it, in the door's own timeline and
// in its hit, and the door lane puts a client's copy at the host's crack with it.
inline constexpr const wchar_t* SetRelativeLocationFn = L"K2_SetRelativeLocation";
inline constexpr const wchar_t* SetRelativeLocationParam = L"NewLocation";
// K2_SetRelativeRotation's rotation parameter, and K2_SetWorldLocation(NewLocation, bSweep, SweepHitResult,
// bTeleport) for a component placed in world space (the ball UFO's `balls`).
inline constexpr const wchar_t* SetRelativeRotationParam = L"NewRotation";
inline constexpr const wchar_t* SetWorldLocationFn = L"K2_SetWorldLocation";
inline constexpr const wchar_t* SetWorldLocationParam = L"NewLocation";
inline constexpr const wchar_t* SetHiddenInGameFn = L"SetHiddenInGame";
inline constexpr const wchar_t* GetComponentLocationFn = L"K2_GetComponentLocation";
inline constexpr const wchar_t* GetComponentForwardFn = L"GetForwardVector";
// The kerfur body-facing sync: the kerfur BP rotates its ACharacter::Mesh world rotation each tick
// to face the local player, which never runs on a tick-less mirror; the host reads
// K2_GetComponentRotation and the mirror is driven with K2_SetWorldRotation
// (Engine.hpp:17941/17950).
inline constexpr const wchar_t* GetComponentRotationFn = L"K2_GetComponentRotation";
inline constexpr const wchar_t* SetWorldRotationFn = L"K2_SetWorldRotation";
inline constexpr const wchar_t* SetRelativeScale3DFn = L"SetRelativeScale3D";
inline constexpr const wchar_t* RelativeRotationProp = L"RelativeRotation";
inline constexpr const wchar_t* RelativeScale3DProp = L"RelativeScale3D";

// Component destruction (UActorComponent::K2_DestroyComponent): removes the local-only systems a
// remote pawn must not own (its PostProcessComponent grades the local screen).
inline constexpr const wchar_t* ActorComponentClass = L"ActorComponent";
inline constexpr const wchar_t* DestroyComponentFn = L"K2_DestroyComponent";
inline constexpr const wchar_t* PostProcessComponentClass = L"PostProcessComponent";

// The world-space nameplate: a translucent UWidgetComponent (TextRender cannot do partial alpha)
// rendering our own UMG (UUserWidget + UTextBlock through SpawnObject) to a render target
// composited with Widget3DPassThrough_Translucent; BlendMode must be set Transparent (the ctor
// default is Masked).
inline constexpr const wchar_t* WidgetComponentClass = L"WidgetComponent";
inline constexpr const wchar_t* AddComponentByClassFn = L"AddComponentByClass";      // on Actor
inline constexpr const wchar_t* FinishAddComponentFn = L"FinishAddComponent";        // on Actor
inline constexpr const wchar_t* SetTintColorAndOpacityFn = L"SetTintColorAndOpacity";// FLinearColor
inline constexpr const wchar_t* RequestRedrawFn = L"RequestRedraw";                  // sets bRedrawRequested
inline constexpr const wchar_t* RequestRenderUpdateFn = L"RequestRenderUpdate";      // forces render-state/RT refresh
inline constexpr const wchar_t* SetComponentTickEnabledFn = L"SetComponentTickEnabled";  // on UActorComponent -- a runtime-added WidgetComponent doesn't tick -> never draws its RT
inline constexpr const wchar_t* IsComponentTickEnabledFn = L"IsComponentTickEnabled";  // on UActorComponent -- a park restarts only a movement it stopped
inline constexpr const wchar_t* NameplateSetTextFn = L"SetText";                     // UTextBlock::SetText(FText)
inline constexpr const wchar_t* TextBlockClass = L"TextBlock";
inline constexpr const wchar_t* ImageClass = L"Image";                              // UImage -- every frame, fill and scrim we build
inline constexpr const wchar_t* KismetTextLibraryClass = L"KismetTextLibrary";
inline constexpr const wchar_t* ConvStringToTextFn = L"Conv_StringToText";           // FString -> FText
// The world-space WidgetComponent's translucent material (a runtime-added component may have the
// slot null, a blank quad); bIsTwoSided=true uses the two-sided slot.
inline constexpr const wchar_t* Widget3DTranslucentMatName = L"Widget3DPassThrough_Translucent";
inline constexpr const wchar_t* Widget3DTranslucentOneSidedMatName = L"Widget3DPassThrough_Translucent_OneSided";
inline constexpr const wchar_t* MaterialInstanceConstantClass = L"MaterialInstanceConstant";  // class of those MICs

// The nameplate is our own UMG (no cooked widget reused): NewObject is the reflected
// UGameplayStatics::SpawnObject(objectClass, Outer); a UUserWidget -> UWidgetTree -> UTextBlock
// root, then UWidgetComponent::SetWidget.
inline constexpr const wchar_t* SpawnObjectFn = L"SpawnObject";        // on GameplayStatics: (objectClass, Outer)->UObject*
inline constexpr const wchar_t* UserWidgetClass = L"UserWidget";
inline constexpr const wchar_t* WidgetTreeClass = L"WidgetTree";
// The game's full-screen widgets a revive leaves to be read: the death chain's black screen and the blood-loss blur.
inline constexpr const wchar_t* BlackScreenWidgetClass = L"blackScreen_C";
inline constexpr const wchar_t* BloodLossBlurWidgetClass = L"ui_bloodLossBlur_C";
// The two-line nameplate (the nick and a separately coloured health bar): a UVerticalBox root
// stacks two UTextBlocks so each carries its own ColorAndOpacity; AddChildToVerticalBox returns
// the slot, and the default Fill slot plus centre justification needs no slot writes.
inline constexpr const wchar_t* VerticalBoxClass = L"VerticalBox";
inline constexpr const wchar_t* AddChildToVerticalBoxFn = L"AddChildToVerticalBox";  // UVerticalBox::AddChildToVerticalBox(UWidget*)->UVerticalBoxSlot*
inline constexpr const wchar_t* SetWidgetFn = L"SetWidget";            // UWidgetComponent::SetWidget(UUserWidget*)
inline constexpr const wchar_t* FontName = L"Roboto";                  // /Engine/EngineFonts/Roboto.Roboto
inline constexpr const wchar_t* FontClassName = L"Font";
inline constexpr const wchar_t* MenuFontName = L"font_ui";             // /Game/main/fonts/font_ui (Share Tech Mono), the game's menu and subtitle font, size 16 on the menu labels
// The screen-space HUD feed (a UUserWidget added to the viewport, not a world WidgetComponent).
inline constexpr const wchar_t* AddToViewportFn = L"AddToViewport";        // UUserWidget::AddToViewport(int32 ZOrder)
inline constexpr const wchar_t* RemoveFromViewportFn = L"RemoveFromViewport";  // UUserWidget::RemoveFromViewport()
inline constexpr const wchar_t* WidgetClass = L"Widget";                   // owns SetVisibility (FindFunction = owning class, no super walk)
inline constexpr const wchar_t* WidgetSetVisibilityFn = L"SetVisibility";  // UWidget::SetVisibility(ESlateVisibility); HitTestInvisible=3
inline constexpr const wchar_t* SetPositionInViewportFn = L"SetPositionInViewport";    // UUserWidget(FVector2D Position, bool bRemoveDPIScale)
inline constexpr const wchar_t* SetAlignmentInViewportFn = L"SetAlignmentInViewport";  // UUserWidget(FVector2D Alignment)
// The client's pause-menu "Save Game" button grey-out; all owned by UWidget, resolved on
// WidgetClass.
inline constexpr const wchar_t* WidgetSetIsEnabledFn = L"SetIsEnabled";       // UWidget::SetIsEnabled(bool bInIsEnabled) -- blocks input + semantic disable (NOT a raw 0xB4 write: packed bitfield)
inline constexpr const wchar_t* WidgetSetRenderOpacityFn = L"SetRenderOpacity";  // UWidget::SetRenderOpacity(float InOpacity) -- brush-independent visual dim
inline constexpr const wchar_t* WidgetGetIsEnabledFn = L"GetIsEnabled";       // UWidget::GetIsEnabled()->bool ReturnValue (read-back diagnostic)
inline constexpr const wchar_t* UiMenuClass = L"ui_menu_C";                   // VOTV unified main+pause menu widget (GameInstance-owned, one live instance)
inline constexpr const wchar_t* UiMenuButtonSaveProp = L"button_Save";        // UButton* @ ui_menu_C+0x2D0 (the "Save Game" button)
inline constexpr const wchar_t* UiMenuIsPauseProp = L"isPause";               // bool @ ui_menu_C+0x4C0 (true => the in-game pause menu, not the main menu)

// The menu button: the UMG classes and UFunctions that construct a UButton at runtime inside the
// live menu. Class names are UE FNames (no U prefix); each UFunction resolves on its owning class.
inline constexpr const wchar_t* ButtonClass = L"Button";                      // UButton
inline constexpr const wchar_t* PanelWidgetClass = L"PanelWidget";            // UPanelWidget (owns Slots/ClearChildren; base of CanvasPanel/HBox/VBox)
inline constexpr const wchar_t* ContentWidgetClass = L"ContentWidget";        // UContentWidget (owns SetContent; UButton is-a ContentWidget)
inline constexpr const wchar_t* SetContentFn = L"SetContent";                 // UContentWidget::SetContent(UWidget*)->UPanelSlot*
inline constexpr const wchar_t* GetContentFn = L"GetContent";                 // UContentWidget::GetContent() -> UWidget*
inline constexpr const wchar_t* WidgetIsHoveredFn = L"IsHovered";             // UWidget::IsHovered()->bool ReturnValue
inline constexpr const wchar_t* ClearChildrenFn = L"ClearChildren";           // UPanelWidget::ClearChildren() -- detach all children (objects survive); for the insert-at-top reorder

// The native main-menu version label: our own UTextBlock injected as the top row of the
// VerticalBox that holds the game's label rows (txt_version's HorizontalBox row sits in
// VerticalBox_138), so the coop version line reads as one more native label and shows and hides
// with the menu.
inline constexpr const wchar_t* UiMenuTxtVersionProp = L"txt_version";        // UTextBlock* @ ui_menu_C+0x430 (the version label -- our anchor)
inline constexpr const wchar_t* TextBlockSetColorFn = L"SetColorAndOpacity";  // UTextBlock::SetColorAndOpacity(FSlateColor) -- the SETTER; a raw ColorAndOpacity write after Slate construction never propagates (UMG bakes props at attach)
// ui_menu_C fields for the inject, resolved by FindPropertyOffset (recook-robust).
inline constexpr const wchar_t* UiMenuButtonStartProp = L"button_start";      // UButton* @ +0x2E0 (NEW GAME). The inject derives its parent VerticalBox from this button's slot and clones its FButtonStyle, tints and VBox slot layout (engine_widget.cpp); the four brushes carry no ResourceObject, so no texture loads. The label style is not cloned (null at some inject timings) but set to measured constants.
inline constexpr const wchar_t* MainPlayerEscapeFn = L"InpActEvt_Escape_K2Node_InputKeyEvent_0";  // engine input event that opens the pause menu (ProcessEvent-dispatched, same class as the flashlight InpActEvt_* we already observe)
// ui_menu_C's click handler of its "Main menu" button: its ubergraph block runs lib_C::loadLevel("menu")
// unless an event is active or the camera is out of bounds.
inline constexpr const wchar_t* PauseMenuQuitClickFn = L"BndEvt__button_Menu_K2Node_ComponentBoundEvent_5_OnButtonClickedEvent__DelegateSignature";
// Head-bone anchoring (USceneComponent::GetSocketLocation, world; the bones are enumerated to find
// the head).
inline constexpr const wchar_t* GetSocketLocationFn = L"GetSocketLocation";  // (FName)->FVector (world)
inline constexpr const wchar_t* GetNumBonesFn = L"GetNumBones";
inline constexpr const wchar_t* GetBoneNameFn = L"GetBoneName";              // (int32)->FName

// Physics-prop pickup: the hook surface is the engine-native UPhysicsHandleComponent UFunctions.
// The BP-side verbs on mainPlayer_C (smoothGrab, pickupObject, dropGrabObject, ...) are BP-pure
// and inlined by their callers, so they never appear as ProcessEvent's function. What does
// dispatch during a grab: InpActEvt_use (the E press), grab__UpdateFunc / grab__FinishedFunc (the
// `grab` timeline), GrabComponentAtLocation* (pickup), SetTargetLocation* (every tick),
// ReleaseComponent (drop).

// The UPhysicsHandleComponent UFunctions (the light-grab path): ProcessEvent-dispatched, matched
// by the UFunction* resolved through reflection::FindFunction on the class.
inline constexpr const wchar_t* PhysicsHandleComponentClass            = L"PhysicsHandleComponent";
inline constexpr const wchar_t* GrabComponentAtLocationFn              = L"GrabComponentAtLocation";
inline constexpr const wchar_t* SetTargetLocationFn                    = L"SetTargetLocation";
inline constexpr const wchar_t* SetTargetLocationAndRotationFn         = L"SetTargetLocationAndRotation";
inline constexpr const wchar_t* ReleaseComponentFn                     = L"ReleaseComponent";

// The UPhysicsConstraintComponent UFunctions (the heavy-drag path: mainPlayer_C.heavyGrab is a
// constraint, distinct from grabHandle); also ProcessEvent-dispatched.
inline constexpr const wchar_t* PhysicsConstraintComponentClass        = L"PhysicsConstraintComponent";
inline constexpr const wchar_t* SetConstrainedComponentsFn             = L"SetConstrainedComponents";
inline constexpr const wchar_t* BreakConstraintFn                      = L"BreakConstraint";

// UPrimitiveComponent::AddImpulse(FVector, FName BoneName, bool bVelChange): the throw signal; the
// BP-inlined throwHoldingProp must call it on the released prop's component.
inline constexpr const wchar_t* PrimitiveComponentClass                = L"PrimitiveComponent";
inline constexpr const wchar_t* AddImpulseFn                           = L"AddImpulse";

// Secondary, the input level: the grabbed prop's identity is read from mainPlayer_C.grabbing_actor
// inside the InpActEvt observer. The "_41" K2Node ordinal is version-fragile (a BP recook can
// renumber it).
// 0.10.0: _37 is the E press (drops a grab, then lookAt / timeuse / useSelectedAction); _38 the release
// (useAction, playerGrabbed); _34 a second press that traces and calls player_use. 0.9.0n had them
// as _41, _42 and _38.
inline constexpr const wchar_t* MainPlayerUseInputEventFn    = L"InpActEvt_use_K2Node_InputActionEvent_37";
// The LMB (fire) action: the native throw-when-holding seam (InpActEvt_fire -> throwHoldingProp),
// separate from use/E. Two handlers (_58 press, _59 release); the client registers both and gates
// on ClientCarryEid so only the edge that finds a live carry acts.
inline constexpr const wchar_t* MainPlayerFireInputEventFn58 = L"InpActEvt_fire_K2Node_InputActionEvent_58";
// ULightComponent::SetIntensity(float) (Engine.hpp:13551), inherited by USpotLightComponent: the
// receiver mirrors the sender's intensity on the puppet's light_R with it (the BP toggles
// Intensity, not bVisible); it marks the render state dirty, which a direct field write does not.
inline constexpr const wchar_t* SetIntensityFn      = L"SetIntensity";

// USpotLightComponent's cone-angle setters: both call MarkRenderStateDirty, so the receiver uses
// them to mirror the sender's cone; a direct field write leaves the proxy stale.
inline constexpr const wchar_t* SetOuterConeAngleFn = L"SetOuterConeAngle";
inline constexpr const wchar_t* SetInnerConeAngleFn = L"SetInnerConeAngle";

// The rest of the beam 0.10.0's flashlight sets from the equipped item and its mode, read on the
// sender and mirrored on a puppet's light through the engine's own setters.
inline constexpr const wchar_t* LightComponentClass        = L"LightComponent";
inline constexpr const wchar_t* LocalLightComponentClass   = L"LocalLightComponent";
inline constexpr const wchar_t* LightAttenuationRadiusProp = L"AttenuationRadius";
inline constexpr const wchar_t* LightColorProp             = L"LightColor";
inline constexpr const wchar_t* LightIESTextureProp        = L"IESTexture";
inline constexpr const wchar_t* SetAttenuationRadiusFn     = L"SetAttenuationRadius";
inline constexpr const wchar_t* SetLightColorFn            = L"SetLightColor";
inline constexpr const wchar_t* SetIESTextureFn            = L"SetIESTexture";
// The beam profiles mainPlayer_C's 'Flashlight Update' selects by flashlightMode (0, 1, 2), the
// UTextureLightProfile objects the payload names by index.
inline constexpr const wchar_t* TextureLightProfileClass = L"TextureLightProfile";
inline constexpr const wchar_t* FlashlightIesProfiles[] = {L"complex_ies", L"narrowcomplex_ies", L"narrow10000"};

// The flashlight click is the USoundWave `/Game/audio/effects/flashlight.flashlight`; the receiver
// plays it at the puppet's location through UGameplayStatics::PlaySoundAtLocation. Resolved by
// leaf name `flashlight` filtered by class `SoundWave` (a GUObjectArray walk, done once and
// cached).
inline constexpr const wchar_t* FlashlightClickSoundName = L"flashlight";
inline constexpr const wchar_t* SoundWaveClass           = L"SoundWave";
inline constexpr const wchar_t* PlaySoundAtLocationFn    = L"PlaySoundAtLocation";

// A USoundWave alone has no spatialisation (PlaySoundAtLocation plays 2D without an attenuation
// override), so a USoundAttenuation is constructed at runtime through UGameplayStatics::SpawnObject
// rather than borrowing the game's `att_*` assets; its field offsets are `off::att`.
inline constexpr const wchar_t* SoundAttenuationClass = L"SoundAttenuation";

// 0.10.0 per-material audio. lib_C::physSound is OBSOLETE (its body only raises the in-game hint
// "obsolete function: phys" and its `Data` out-param is gone). The replacement is
// lib_C::getPhysicalAsAudio(PhysicalMaterial, __WorldContext) -> out AudioPhysMat
// (a physMat_SoundsAndParticles_C), out IsAudioPhysical; a material that is not an audio material
// resolves to the physsnd_fallback asset, as native does. The native pickup reads `sound_soft` off
// that object and impacts read `sound_impact`. prop_C's physicsImpact component carries the
// material to look up: physmat_override when valid, else physmat (comp_physicsImpact_C).
inline constexpr const wchar_t* PhysAudioLibClass            = L"lib_C";
inline constexpr const wchar_t* GetPhysicalAsAudioFn         = L"getPhysicalAsAudio";
inline constexpr const wchar_t* GetPhysicalAsAudioMatParam   = L"PhysicalMaterial";
inline constexpr const wchar_t* GetPhysicalAsAudioWorldParam = L"__WorldContext";
inline constexpr const wchar_t* GetPhysicalAsAudioOutParam   = L"AudioPhysMat";
inline constexpr const wchar_t* PhysAudioSoftProp            = L"sound_soft";    // physMat_SoundsAndParticles_C: grab / pickup cue
inline constexpr const wchar_t* PhysAudioImpactProp          = L"sound_impact";  // physMat_SoundsAndParticles_C: impact / land cue
// The break cues of the same object (physSnd_wood_crate: damage_wood_Cue and break_crate_Cue), which
// comp_physicsImpact_C plays inside impactDamage; read from the pak's name map, not yet from a dump.
inline constexpr const wchar_t* PhysAudioDamagedProp         = L"sound_damaged";    // damage taken, still standing
inline constexpr const wchar_t* PhysAudioDestroyedProp       = L"sound_destroyed";  // the break
inline constexpr const wchar_t* PropPhysicsImpactProp        = L"physicsImpact";      // prop_C -> comp_physicsImpact_C
inline constexpr const wchar_t* PhysImpactMatOverrideProp    = L"physmat_override";   // comp_physicsImpact_C UPhysicalMaterial*, wins when valid
inline constexpr const wchar_t* PhysImpactMatProp            = L"physmat";            // comp_physicsImpact_C UPhysicalMaterial*
inline constexpr const wchar_t* PhysImpactDamageFn           = L"impactDamage";       // comp_physicsImpact_C (damage, impact, hit, actor)
// comp_physicsImpact_C's own pick of the material it plays from: physmat_override when valid, else physmat when its
// customPhysmat flag is set, else the physical material of its hit component's first material (an ordinary prop sets
// neither, so reading the two properties alone finds none). Returns that UPhysicalMaterial*.
inline constexpr const wchar_t* PhysImpactCompMatFn          = L"getComponentPhysMat";
inline constexpr const wchar_t* PhysImpactCompMatOutParam    = L"ReturnValue";

// mainPlayer_C drives every ragdoll cause through `ragdollMode(bool ragdoll, bool passOut, bool
// death)` (mainPlayer.hpp:484) and recovers through `forceGetUp()` (line 641). On an unpossessed puppet
// ragdollMode(true,true,false) flips isRagdoll and spawns the ragdoll actor, and forceGetUp clears the AnimBP
// gate; coop/player/player_damage refuses ragdollMode on a peer's puppet, whoever calls it.
inline constexpr const wchar_t* MainPlayerRagdollModeFn = L"ragdollMode";
inline constexpr const wchar_t* MainPlayerForceGetUpFn  = L"forceGetUp";
// `forceWakeup()`: the unconditional stand-up. It jumps straight to the ubergraph block (@25800)
// that restores the movement mode, capsule collision, camera attach and EnableInput and detaches
// the ragdoll mesh, reading no gate; `wakeup(passOut)` early-outs on `dead || isWakingUp ||
// noWakeup` (@26584) and `forceGetUp()` runs a 0.2 s latent Delay first (@39625), so the KO
// respawn calls this one.
inline constexpr const wchar_t* MainPlayerForceWakeupFn = L"forceWakeup";

// The plushie ragdoll the game spawns for a faint or the C key. playerRagdoll_C derives from
// ragdoll_C, which owns the body `SkeletalMesh`; `Player` (the owning mainPlayer_C, Expose On Spawn)
// is playerRagdoll_C's own, and mainPlayer_C keeps the live one in `ragdollActor`. The offsets of all
// three are reflected (ue_wrap/core/reflected_offset.h): a hard-coded 0xC40 for ragdollActor went stale
// in 0.10.0 (the field moved to a different slot) and 0x248 for Player landed inside an array header.
// The kickoff bone is the one ragdollMode's native recipe adds its -100 cm/s forward impulse to.
inline constexpr const wchar_t* PlayerRagdollClass     = L"playerRagdoll_C";
inline constexpr const wchar_t* RagdollBaseClass       = L"ragdoll_C";
inline constexpr const wchar_t* RagdollChestBone       = L"chest";
inline constexpr const wchar_t* MainPlayerRagdollActorProp = L"ragdollActor";   // mainPlayer_C -> its live playerRagdoll_C

// The desk computer's console log: comp_writehecer(line) prepends a line break and the line to the text property
// comp_hecertext, keeps its last 1500 characters and paints the console widget (coop/interactables/comp_sync).
inline constexpr const wchar_t* DeskCompHecerTextProp = L"comp_hecertext";
inline constexpr const wchar_t* DeskCompWriteHecerFn  = L"comp_writehecer";

// The primary player-damage entry: addDamage and punch end in it on the owning peer's own possessed
// mainPlayer_C, so a delivered hit runs through that peer's armour and inventory BP and drops its
// saveSlot.health; coop/player/player_damage refuses it on a peer's puppet. The FName keeps its
// spaces.
inline constexpr const wchar_t* MainPlayerAddPlayerDamageFn = L"Add Player Damage";

// The damage body pulse: the puppet's skin exposes no drivable tint parameter, so the hurt flash
// swaps the body mesh materials to an existing pak material and restores them. It must be a
// skeletal-character material: a static-mesh or particle material has no GPUSkinVertexFactory
// permutation and UE substitutes its default grey. inst_goregibs_organsSK is a gore skin material
// and renders its bloody-red texture on the kerfur; swapped onto both visible body meshes
// (Mesh@0x280 and mesh_playerVisible).
inline constexpr const wchar_t* PlayerHurtFlashMaterialName = L"inst_goregibs_organsSK";
inline constexpr const wchar_t* MaterialInstanceConstantClassName = L"MaterialInstanceConstant";

// A fresh USpotLightComponent is spawned on the puppet through AddComponentByClass +
// FinishAddComponent, the only reflection-callable path that runs
// UActorComponent::RegisterComponent (which creates the FSceneProxy); the class-default light_R's
// SceneProxy stays null under reflection-only writes.
inline constexpr const wchar_t* SpotLightComponentClass = L"SpotLightComponent";

// The autotest's give/equip/battery setup: AmainPlayer_C::addPropToPlayer(FName prop), the
// cheat-menu path (spawns the actor, adds it to the inventory and may auto-equip an equipment
// item).
inline constexpr const wchar_t* MainPlayerAddPropToPlayerFn = L"addPropToPlayer";

// The standard flashlight's battery class (Aprop_batts_C): a TSubclassOf written into
// saveSlot.flashlightBattery marks "a battery is inserted" (a class reference, not an instance).
inline constexpr const wchar_t* BattsClass = L"prop_batts_C";

// The flashlight equipment class (the FName the BP uses to identify the item).
inline constexpr const wchar_t* FlashlightEquipmentClass = L"prop_equipment_flashlight_C";

// The flashlight's battery ticker: one per world, found by the player's BeginPlay and pointed at
// that player (ticker.player = self). Its tick drains the equipped flashlight's battery while
// ticker.player.flashlight is set, so whichever mainPlayer_C began play last owns the drain.
inline constexpr const wchar_t* TickerPlayerFlashlightClass = L"ticker_playerFlashlight_C";

// The UTimelineComponent BP-callable methods (the autotest forces the `grab` timeline so the
// timeline observers fire without a real E press); ProcessEvent-dispatched.
inline constexpr const wchar_t* TimelineComponentClass       = L"TimelineComponent";
inline constexpr const wchar_t* TimelinePlayFromStartFn      = L"PlayFromStart";
// UTimelineComponent's playback verbs, which the garage's settle drives (ue_wrap/devices/garage):
// Stop(), IsPlaying() -> bool, GetTimelineLength() / GetPlaybackPosition() -> float, and
// SetPlaybackPosition(NewPosition, bFireEvents, bFireUpdate), which fires the timeline's update
// delegate without waiting for a tick.
inline constexpr const wchar_t* TimelineStopFn               = L"Stop";
inline constexpr const wchar_t* TimelineIsPlayingFn          = L"IsPlaying";
inline constexpr const wchar_t* TimelineGetLengthFn          = L"GetTimelineLength";
inline constexpr const wchar_t* TimelineGetPositionFn        = L"GetPlaybackPosition";
inline constexpr const wchar_t* TimelineSetPositionFn        = L"SetPlaybackPosition";
inline constexpr const wchar_t* TimelinePlayFn              = L"Play";
inline constexpr const wchar_t* TimelineReverseFn           = L"Reverse";
inline constexpr const wchar_t* TimelineNewPositionParam     = L"NewPosition";
inline constexpr const wchar_t* TimelineFireEventsParam      = L"bFireEvents";
inline constexpr const wchar_t* TimelineFireUpdateParam      = L"bFireUpdate";
// garage_C's swing timeline (a UTimelineComponent member named `move`, as door_C's) and its mid-swing flag.
// door_C's two leaf roots (scene components the leaves attach to; a swing and a hit move these, never the leaves),
// the offset the right root travels to (negated) and the left root travels to, and the two places the move timeline
// lerps each root from, written by doorOpen from where the roots stand and by doorClose as zero.
inline constexpr const wchar_t* DoorRightRootProp            = L"door_r_root";
inline constexpr const wchar_t* DoorLeftRootProp             = L"door_l_root";
inline constexpr const wchar_t* DoorOpenOffsetProp           = L"openDoor";
inline constexpr const wchar_t* DoorLerpaRightProp           = L"lerpa_r";
inline constexpr const wchar_t* DoorLerpaLeftProp            = L"lerpa_l";
// door_C's two flags that make a bypassed verb return without moving the door: doorOpen(bypass) returns on
// `superclosed` (and plays the jam shake on `jammed`), doorClose(bypass) returns on `cantclose`. Both are read
// by the graph; only the save's load writes `superclosed`, and nothing in the captured graphs writes `cantclose`.
inline constexpr const wchar_t* DoorSuperClosedProp          = L"superclosed";
inline constexpr const wchar_t* DoorCantCloseProp            = L"cantclose";
// door_C's physical end state, which the native sequence sets and a root write does not: `coll` is the box that
// blocks the doorway (doorOpen and the open finish set it NoCollision, doorClose and the close finish
// QueryAndPhysics, and the save's settime(opened) sets it from its argument -- betterloaddata passes false), and
// `door_l` / `door_r` are the leaf meshes under the roots (both verbs set them QueryAndPhysics for the swing, the open
// finish NoCollision). `upd` sets the door's nav area from Active and isOpened, no sound.
inline constexpr const wchar_t* DoorBlockerCollProp          = L"coll";
inline constexpr const wchar_t* DoorLeftLeafProp             = L"door_l";
inline constexpr const wchar_t* DoorRightLeafProp            = L"door_r";
inline constexpr const wchar_t* DoorNavUpdateFn              = L"upd";
inline constexpr const wchar_t* GarageMoveTimelineProp       = L"move";
inline constexpr const wchar_t* GarageMovingProp             = L"mov";
// garage_C's door mesh (the skeletal mesh the swing montage plays on), its looping swing sound and its nav
// modifier, and the verb the game's own load runs to put the door at its end (it plays the montage and the timeline).
inline constexpr const wchar_t* GarageMeshProp               = L"SkeletalMesh";
inline constexpr const wchar_t* GarageLoopSoundProp          = L"loop";
inline constexpr const wchar_t* GarageNavModifierProp        = L"navmodifier";
inline constexpr const wchar_t* GarageSetTimeFn              = L"settime";
inline constexpr const wchar_t* GarageSetTimeOpenParam       = L"open";
// The nav area a closed garage blocks with, set by the timeline's finish; the open end clears it.
inline constexpr const wchar_t* GarageClosedNavAreaClass     = L"NavArea_Null";
inline constexpr const wchar_t* NavModifierSetAreaClassFn    = L"SetAreaClass";
inline constexpr const wchar_t* NavModifierAreaClassParam    = L"NewAreaClass";
// UAnimInstance's montage verbs, which put the garage's door mesh at a place in its swing: the montage the instance
// is playing, a jump to the end of a named section, and the montage's position read and set. The section names
// are the montage's own: "open" and "close".
inline constexpr const wchar_t* AnimInstanceClass            = L"AnimInstance";
inline constexpr const wchar_t* AnimGetActiveMontageFn       = L"GetCurrentActiveMontage";
inline constexpr const wchar_t* AnimJumpToSectionsEndFn      = L"Montage_JumpToSectionsEnd";
inline constexpr const wchar_t* AnimMontageGetPositionFn     = L"Montage_GetPosition";
inline constexpr const wchar_t* AnimMontageSetPositionFn     = L"Montage_SetPosition";
inline constexpr const wchar_t* GarageOpenSection            = L"open";
inline constexpr const wchar_t* GarageCloseSection           = L"close";
// UActorComponent::SetActive(bNewActive, bReset), which turns the garage's swing sound off at rest.
inline constexpr const wchar_t* ComponentSetActiveFn         = L"SetActive";

// The Aprop_C BP class name (the prop registry's GUObjectArray scan).
inline constexpr const wchar_t* PropClass             = L"prop_C";

// `Aprop_C.thrown(AmainPlayer_C* Player)`: the throw event the prop's BP wires to the whoosh sound
// and the particle trail; the receiver calls it on release with a non-zero impulse so the effects
// fire as for a local throw (prop.hpp:166).
inline constexpr const wchar_t* PropThrownFn          = L"thrown";

// Aprop_C.Init: the BP construction script that runs on every Aprop spawn (the FinishSpawningActor
// path). A POST hook on the base class catches every derivative's spawn (mushroom growth, save
// load, inventory drops); the host broadcasts, the client skips.
inline constexpr const wchar_t* PropInitFn            = L"Init";

// trashBitsPile_C's collect counters (declared on the pile class, 0.10.0 Blueprint capture), resolved
// by reflection and required to be IntProperty before any read or write; the 0.9.0n offsets are a
// UObject* in 0.10.0 and are never used.
inline constexpr const wchar_t* TrashPileCounterA     = L"amountA";
inline constexpr const wchar_t* TrashPileCounterB     = L"amountB";
inline constexpr const wchar_t* TrashPileCounterType  = L"IntProperty";

// Classes that are intermediate state of a host-authoritative pipeline. Mushrooms grow through
// class identity, mushroom7_C (growing, hidden, collision off) to a mature class through a
// timer-driven Transform. On a client a local spawn of these is destroyed on Init POST and a wire
// PropSpawn for them is dropped; the mature variant arrives through the host's broadcast. Each
// entry is the BP class FName.
inline constexpr const wchar_t* PropMushroomGrowingClass = L"prop_food_mushroom7_C";

// On the receiver Aprop_C.setKey is dispatched between Begin and Finish so the prop's Init() does not
// overwrite Key with NewGuid.
inline constexpr const wchar_t* PropSetKeyFn            = L"setKey";

// UKismetStringLibrary::Conv_StringToName(const FString&) -> FName (BlueprintPure, dispatched on
// the CDO): converts the wire Key string into a live FName for Aprop_C.setKey before
// FinishSpawningActor runs Init().
inline constexpr const wchar_t* KismetStringLibraryClass = L"KismetStringLibrary";
inline constexpr const wchar_t* ConvStringToNameFn       = L"Conv_StringToName";

// The UPrimitiveComponent physics UFunctions (exec thunks SetPhysicsLinearVelocity 0x1430DFA40,
// SetPhysicsAngularVelocityInDegrees 0x1430DF7B0, GetPhysicsLinearVelocity 0x1430DC550,
// GetPhysicsAngularVelocityInDegrees 0x1430DC320): the host reads the held mesh's velocity at the
// release edge and the receiver writes it after SetSimulatePhysics(true), so the body re-enters
// the simulation with the launch state.
inline constexpr const wchar_t* SetSimulatePhysicsFn                 = L"SetSimulatePhysics";
inline constexpr const wchar_t* IsSimulatingPhysicsFn                = L"IsSimulatingPhysics";
inline constexpr const wchar_t* GetPhysicsLinearVelocityFn           = L"GetPhysicsLinearVelocity";
inline constexpr const wchar_t* GetPhysicsAngularVelocityInDegreesFn = L"GetPhysicsAngularVelocityInDegrees";
inline constexpr const wchar_t* SetPhysicsLinearVelocityFn           = L"SetPhysicsLinearVelocity";
inline constexpr const wchar_t* SetPhysicsAngularVelocityInDegreesFn = L"SetPhysicsAngularVelocityInDegrees";
// GetCenterOfMass: a body's centre of mass in world space, a welded body's the whole weld's.
inline constexpr const wchar_t* GetCenterOfMassFn                    = L"GetCenterOfMass";

// AdaynightCycle_C: the singleton weather authority (the scheduler timers, the state fields, the
// mutator UFunctions), resolved through R::FindObjectByClass.
inline constexpr const wchar_t* DaynightCycleClass = L"daynightCycle_C";

// AlightningStrike_C: the one-shot strike actor (three audio components, a point light, a sphere
// collider, a timeline self-destruct); its location is its own transform. The receiver spawns it
// through the deferred pair.
inline constexpr const wchar_t* LightningStrikeClass = L"lightningStrike_C";

// AsuperFog_C: the dense "super fog" event actor. Not stored on the cycle; its existence is its
// active state. daynightCycle_C::superFogEvent spawns it at the identity transform; its BeginPlay
// (after a 1 s Delay) registers it with lib_C::setEvent, caches the cycle in cyc, enables its tick
// and calls spawnUfo (a HoelUfo_C 500 uu above a navmesh-projected point, held in ufo); its tick
// adds dt/duration to alpha and writes cyc.thickFog = lerp(0, thickness, sin(alpha*180)^3), and
// past alpha 1 zeroes thickFog, destroys the ufo and itself. The four properties are reflected
// (FindPropertyOffset is case-insensitive).
// Anewsky_C::tp, the earthTp event (trigger_eventer_C::runEvent('earthTp') calls it on the cycle's
// skysphere): a latent sequence -- setEvent, PlaySound2D(earthTP_in, 1.5, 0.4), 20 s, light_blackhole
// visible + skyVisibility(true) (the black-hole sky), 6 s, an email, 60 s, PlaySound2D(earthTP_out,
// 1.5, 0.4), 20 s, then unless the permanent-black-hole gamerule holds, light_blackhole hidden +
// skyVisibility(false), and two emails. skyVisibility(bNewHidden) writes skyHidden, the state.
inline constexpr const wchar_t* NewSky_TpFn              = L"tp";
inline constexpr const wchar_t* NewSky_SkyVisibilityFn   = L"skyVisibility";
inline constexpr const wchar_t* NewSky_SkyVisibilityParam = L"bNewHidden";
inline constexpr const wchar_t* NewSky_SkyHiddenProp     = L"skyHidden";
inline constexpr const wchar_t* NewSky_LightBlackHoleProp = L"light_blackhole";
// The tp's two 2D sounds (/Game/audio/effects; the asset class is tried as SoundWave, then SoundCue).
inline constexpr const wchar_t* EarthTpInSoundPath  = L"/Game/audio/effects/earthTP_in.earthTP_in";
inline constexpr const wchar_t* EarthTpOutSoundPath = L"/Game/audio/effects/earthTP_out.earthTP_out";
inline constexpr float          EarthTpSoundVolume  = 1.5f;
inline constexpr float          EarthTpSoundPitch   = 0.4f;
inline constexpr float          EarthTpOutDelaySec  = 86.f;  // 20 + 6 + 60 s from the tp's start

inline constexpr const wchar_t* SuperFogClass          = L"superFog_C";
inline constexpr const wchar_t* SuperFog_AlphaProp     = L"alpha";
inline constexpr const wchar_t* SuperFog_DurationProp  = L"duration";
inline constexpr const wchar_t* SuperFog_ThicknessProp = L"thickness";
inline constexpr const wchar_t* SuperFog_UfoProp       = L"ufo";
// AHoelUfo_C: the super fog's UFO. A 1 s looping attack timer sphere-overlaps 8000 uu around it
// and, per actor at 1 %, mints a hoelUfoAttack_C beam that ignites and damages its target; 5 s and
// 10 s timers re-roll its speed and heading. Host-run; mirrored on the world-actor lane.
inline constexpr const wchar_t* HoelUfoClass           = L"HoelUfo_C";

// AdirectionalWind_C: the wind actor (a singleton), resolved by class like the cycle
// (coop/weather_sync through ue_wrap/directionalwind).
inline constexpr const wchar_t* DirectionalWindClass = L"directionalWind_C";

// The scheduler UFunctions: the client intercepts them (a PRE cancel) so it never decides "rain
// now"; the host's POST observer reads the post-mutation cycle state and broadcasts WeatherState.
inline constexpr const wchar_t* DaynightCycle_timerRainFn       = L"timerRain";
inline constexpr const wchar_t* DaynightCycle_superFogEventFn   = L"superFogEvent";

// The mutator UFunctions the receiver invokes to apply the host's state: causeRain(bool) starts
// or stops the visible rain; setRainProperties(bool, 4 floats) writes the scalar block and drives
// setRainParameters; intComs_triggerSnow fans out to 53 BP listeners (a direct field write would
// miss them); setWindParameters() reads the cycle state into AdirectionalWind_C; spawnFog spawns
// the rolling-fog actor (mirror-spawned by coop/weather_fog, suppressed on the client).
// SetFogDensity is only the dev fogprobe's.
inline constexpr const wchar_t* DaynightCycle_causeRainFn          = L"causeRain";
inline constexpr const wchar_t* DaynightCycle_setRainPropertiesFn  = L"setRainProperties";
inline constexpr const wchar_t* DaynightCycle_setWindParametersFn  = L"setWindParameters";
inline constexpr const wchar_t* DaynightCycle_intComsTriggerSnowFn = L"intComs_triggerSnow";
inline constexpr const wchar_t* DaynightCycle_spawnFogFn           = L"spawnFog";
inline constexpr const wchar_t* DaynightCycle_setFogDensityFn      = L"SetFogDensity";  // dev fogprobe only
// setRainParticles directly activates or deactivates the cycle's rainEffect
// UParticleSystemComponent; causeRain does not always activate it, so the receiver calls this
// after causeRain.
inline constexpr const wchar_t* DaynightCycle_setRainParticlesFn   = L"setRainParticles";

// The red sky's toggle on AmainGamemode_C: spawnRedSky() destroys the redSkyEvent_C held at
// mainGamemode.redSky @0x0888 (mainGamemode.hpp:150) when one lives and spawns one there otherwise
// (ue_wrap/world/red_sky).
inline constexpr const wchar_t* MainGamemode_SpawnRedSkyFn      = L"spawnRedSky";
// The two weather-event birth classes (weather_event_births' client birth catch).
inline constexpr const wchar_t* WeatherFogControllerClass      = L"weatherFogController_C";
inline constexpr const wchar_t* BlackFogClass                  = L"blackFog_C";
// AblackFog_C: mainGamemode_C::spawnBlackFog spawns it at the identity transform (the clock's hour
// roll at 0.05 %, the cheat menu). Its BeginPlay ramps `a` by dt/spd to 1 (set() clamps it and
// drives the post-process alpha, the birds and the ambience), then writes a = 180 and counts it down
// by dt as the hold, then a = 1, destroys its eyes and fades a by dt to 0 and destroys itself. A
// 40 s timer runs spawnghost while a >= 1: an eyer_C on the navmesh within 20000 uu of the origin,
// kept in `eyes`. Both properties are reflected (FindPropertyOffset is case-insensitive).
inline constexpr const wchar_t* BlackFog_AProp                 = L"a";
inline constexpr const wchar_t* BlackFog_SpdProp               = L"spd";
// SetCollisionEnabled (UPrimitiveComponent): remote_prop::OnSpawn restores default collision
// (QueryAndPhysics=3) on wire-converged props whose local copy a natural-spawn pipeline left at
// NoCollision. The param is `NewType` (ECollisionEnabled::Type).
inline constexpr const wchar_t* SetCollisionEnabledFn                = L"SetCollisionEnabled";
// SetCollisionResponseToChannel (UPrimitiveComponent; params `Channel` ECollisionChannel, `NewResponse`
// ECollisionResponse): the player puppet's capsule ignores PhysicsBody so it never shoves a simulated prop.
inline constexpr const wchar_t* SetCollisionResponseToChannelFn      = L"SetCollisionResponseToChannel";
// GetCollisionResponseToChannel (UPrimitiveComponent; param `Channel`, returns ECollisionResponse): the
// read-back that confirms the puppet guard's Ignore landed.
inline constexpr const wchar_t* GetCollisionResponseToChannelFn      = L"GetCollisionResponseToChannel";
// GetCollisionObjectType (UPrimitiveComponent; no params, returns ECollisionChannel): the object channel a
// component itself sits on. The prop-channel census reads the root of each prop class with it, to show which
// channel (WorldDynamic or PhysicsBody) a heavy prop collides as, so the puppet guard's list is proven.
inline constexpr const wchar_t* GetCollisionObjectTypeFn             = L"GetCollisionObjectType";
// UCharacterMovementComponent::bEnablePhysicsInteraction (a bool bitfield, resolved at run time through
// FindBoolProperty): off, CapsuleTouched returns before it pushes any simulated body the capsule touches.
inline constexpr const wchar_t* CmcEnablePhysicsInteractionProp      = L"bEnablePhysicsInteraction";
// The other UCharacterMovementComponent knobs that set how hard a capsule pushes a simulated body (all float
// properties, found by name and type through reflection::PropertyOffsetAndClass): the push force factor, the
// initial push factor, the touch force factor, and the downward force a standing capsule puts on the body under
// it. coop/player/local_push scales them on the local player's own pawn for the length of a session.
inline constexpr const wchar_t* CmcPushForceFactorProp               = L"PushForceFactor";
inline constexpr const wchar_t* CmcInitialPushForceFactorProp        = L"InitialPushForceFactor";
inline constexpr const wchar_t* CmcTouchForceFactorProp              = L"TouchForceFactor";
inline constexpr const wchar_t* CmcStandingDownwardForceScaleProp    = L"StandingDownwardForceScale";
// ACharacter's FBasedMovementInfo StructProperty: what the character stands on (its MovementBase first). Read by
// character_push::MovementBaseOf so prop sync knows which prop a player stands on.
inline constexpr const wchar_t* CharacterBasedMovementProp           = L"BasedMovement";
// The cap-mushroom class: it overrides Aprop_C::Init and its natural-spawn path
// (AmushroomSpawner_C::Spawn -> spawnedNaturally()) sets its StaticMesh to NoCollision.
// mushroom7_C grows into prop_puffballMature_C, a separate species.
inline constexpr const wchar_t* PropFoodMushroomClass                = L"prop_food_mushroom_C";
// The wall-vent cover (props-table row vent) and the key of the one the ventCrawler event pulls down.
inline constexpr const wchar_t* PropVentClass                        = L"prop_vent_C";
inline constexpr const wchar_t* PropVentHangingKey                   = L"ventHanging";
// ventCrawler_C: the object variable holding the vent it pulls down (an ObjectProperty, bound in
// ReceiveBeginPlay from getObjectFromKey('ventHanging')), and the break timeline's two bodies
// (ventbreakTL: Update sets the vent's transform each frame; Finished runs setPropProps(static false,
// frozen true, active false, sleep false) on it).
inline constexpr const wchar_t* VentCrawlerVentProp                  = L"vent";

// prop_glowstick_C (0.10.0), the glowstick and its coloured subclasses. It does not carry comp_wallAttachable: it
// hangs through its own members. While held its tick traces forward (`ray`); on a hit `stickto(hit, false)` sets
// `sticked`, `loc` (the impact point), `stickstatic` (the hit component does not simulate), `stickcomp`, moves the
// `hook` scene component to `loc`, and runs `hookdelay`, whose body (after a zero delay) constrains `hook` to the
// stick's mesh through the `physicsconstraint` component and sets the constraint frames (`setframe`). It sets
// neither the base prop's frozen nor its Static. `releaseglowstick` breaks the constraint and clears `sticked`; every
// grab verb of the stick runs it first. Members are read from the pak's bytecode.
inline constexpr const wchar_t* GlowstickClass                       = L"prop_glowstick_C";
inline constexpr const wchar_t* GlowstickStickedProp                 = L"sticked";
inline constexpr const wchar_t* GlowstickStickStaticProp             = L"stickstatic";
inline constexpr const wchar_t* GlowstickLocProp                     = L"loc";
inline constexpr const wchar_t* GlowstickHookProp                    = L"hook";
inline constexpr const wchar_t* GlowstickStickToFn                   = L"stickto";
inline constexpr const wchar_t* GlowstickHookDelayFn                 = L"hookdelay";
inline constexpr const wchar_t* GlowstickReleaseFn                   = L"releaseglowstick";

// AoutsideChurch_C (0.10.0): the level-placed Stolas church. Its state is four save fields
// (getData's mbool [broken, angry, striked] and mint [harvests]); updBroken and updEyes are the
// presentation refreshes loadData runs after writing them (coop/world/church_sync).
inline constexpr const wchar_t* OutsideChurchClass         = L"outsideChurch_C";
inline constexpr const wchar_t* OutsideChurch_BrokenProp   = L"broken";
inline constexpr const wchar_t* OutsideChurch_AngryProp    = L"angry";
inline constexpr const wchar_t* OutsideChurch_StrikedProp  = L"striked";
inline constexpr const wchar_t* OutsideChurch_HarvestsProp = L"harvests";
inline constexpr const wchar_t* OutsideChurch_UpdBrokenFn  = L"updBroken";
inline constexpr const wchar_t* OutsideChurch_UpdEyesFn    = L"updEyes";
inline constexpr const wchar_t* OutsideChurch_EndOverlapOtherActorParam = L"OtherActor";
// The church's and the focus actor's watched verbs are fact rows in coop/world/church_sync.

// The 0.10.0 UFO encounters (main map untitled_1). Each controller is level-placed and mints its
// flock through a bytecode-internal BeginDeferred; the products ride the world-actor lane
// (kWorldActorAllowlist), their producers are refused on a client (coop/world/spawn_authority).
inline constexpr const wchar_t* UfoKnobControllerClass  = L"ufo_knob_controller_C";
inline constexpr const wchar_t* UfoUkControllerClass    = L"ufo_UK_controller_C";
inline constexpr const wchar_t* UfoUkControllerEnemyClass = L"ufo_UK_controller_enemy_C";
inline constexpr const wchar_t* UfoKnobClass            = L"ufo_knob_C";
inline constexpr const wchar_t* UfoUkClass              = L"ufo_UK_C";
inline constexpr const wchar_t* UfoUkEnemyClass         = L"ufo_UK_enemy_C";

// The SAT console's terminal, ui_console_C (0.10.0 bytecode capture). One instance per machine is the desk
// atlas's umg_console; the host also makes hidden ones, one per typist. enterCommand(c) runs a line,
// writeToLog(B, bracketsType) appends a printed line to consoleLine, fixLog keeps its rightmost 3000
// characters and repaints, upd redraws the input (text_command = command + "|"), init(name, hide, dish)
// points it at a dish. The live input is the EditableTextBox's text; the alternate keyboard path
// (OnKeyDown_0 -> typeSymbol) appends to `command` instead. Every property is resolved by name AND type
// (reflection::PropertyOffsetAndClass) and logged once on a miss; the watched function names are rows in
// coop/interactables/sat_console_sync, whose strings must be literals.
inline constexpr const wchar_t* SatConsoleClass            = L"ui_console_C";
inline constexpr const wchar_t* SatConsoleEnterCommandFn   = L"enterCommand";
inline constexpr const wchar_t* SatConsoleWriteToLogFn     = L"writeToLog";
inline constexpr const wchar_t* SatConsoleInitFn           = L"init";
inline constexpr const wchar_t* SatConsoleUbergraphFn      = L"ExecuteUbergraph_ui_console";
inline constexpr const wchar_t* SatConsoleFixLogFn         = L"fixLog";
inline constexpr const wchar_t* SatConsoleUpdFn            = L"upd";
inline constexpr const wchar_t* SatConsoleProcessingProp   = L"processing";
inline constexpr const wchar_t* SatConsolePanelProp        = L"panel";
inline constexpr const wchar_t* SatConsoleUsedProp         = L"used";
inline constexpr const wchar_t* SatConsoleActiveDishProp   = L"activeDish";
inline constexpr const wchar_t* SatConsoleNameProp         = L"name";
inline constexpr const wchar_t* SatConsoleCommandProp      = L"command";       // FString: the typed line
inline constexpr const wchar_t* SatConsoleTextBoxProp      = L"EditableTextBox";
inline constexpr const wchar_t* SatConsoleLogProp          = L"consoleLine";   // FString: the printed history
inline constexpr const wchar_t* SatConsoleAtlasClass       = L"ui_consolesAtlas_C";
inline constexpr const wchar_t* SatConsoleAtlasTerminalProp = L"umg_console";
inline constexpr const wchar_t* SatConsoleDeskClass        = L"analogDScreenTest_C";
inline constexpr const wchar_t* SatConsoleDeskPanelsProp   = L"consoles";
// The SAT console's panels, panel_SATconsole_C (0.10.0 bytecode capture): every panel shows the one terminal
// (`Widget`, the atlas's umg_console the desk hands it). con() points that terminal at the panel's dish:
// init(control_asDish.nameDish, false, control_asDish) when control_asDish is valid, init("ROOT", false, none)
// otherwise. The `root` flag marks the desk's own panel (its begin makes it gamemode.rootConsole). con() runs on a
// use (actionOptionIndex) and on any mainPlayer_C's begin-overlap of the panel's trigger (the bound event named
// below, whose OtherActor is the actor that walked in).
inline constexpr const wchar_t* SatPanelClass              = L"panel_SATconsole_C";
inline constexpr const wchar_t* SatPanelRootProp           = L"root";            // bool
inline constexpr const wchar_t* SatPanelDishProp           = L"control_asDish";  // dish_C*
inline constexpr const wchar_t* SatPanelWidgetProp         = L"Widget";          // ui_console_C*
inline constexpr const wchar_t* SatPanelConFn              = L"con";
inline constexpr const wchar_t* SatPanelOverlapFn          =
    L"BndEvt__updName_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature";
inline constexpr const wchar_t* SatPanelOverlapActorParam  = L"OtherActor";
inline constexpr const wchar_t* DishClass                  = L"dish_C";
inline constexpr const wchar_t* DishNameProp               = L"nameDish";        // FText: the prompt con() gives
inline constexpr const wchar_t* EditableTextBoxClass       = L"EditableTextBox";
inline constexpr const wchar_t* EditableTextBoxSetTextFn   = L"SetText";
inline constexpr const wchar_t* EditableTextBoxGetTextFn   = L"GetText";
// The desk's widget chain, as the console repair reads it: analogDScreenTest_C.Widget is the atlas the desk's own
// WidgetComponent holds (the component is found by class and by the desk being its outer, not by a guessed name), and
// the atlas's umg_console is the terminal, a child of its widget tree named alike. GetUserWidgetObject is the
// component's reflected reader of the widget it shows (GetWidget is asked after it, for a build that names it so). The
// tick-offscreen pair is the component's: the setter is driven only when its one-bool signature validates and the
// prior value reads back (the function, or bTickWhenOffscreen by reflection), so it can be restored.
inline constexpr const wchar_t* SatConsoleDeskWidgetProp   = L"Widget";
inline constexpr const wchar_t* WidgetComponentGetWidgetFn = L"GetUserWidgetObject";
inline constexpr const wchar_t* WidgetComponentGetWidgetAltFn = L"GetWidget";
inline constexpr const wchar_t* WidgetComponentSetTickWhenOffscreenFn = L"SetTickWhenOffscreen";
inline constexpr const wchar_t* WidgetComponentGetTickWhenOffscreenFn = L"GetTickWhenOffscreen";
inline constexpr const wchar_t* WidgetComponentTickWhenOffscreenProp  = L"bTickWhenOffscreen";
inline constexpr const wchar_t* WidgetComponentSetTickModeFn          = L"SetTickMode";

// The food family (ue_wrap/actors/food; 0.10.0 cook, read from prop_food_C's, prop_cookingFood_C's and
// prop_food_shrimp_C's bytecode). prop_food_C::ReceiveTick lerps `temperature` (kelvin) toward the room, derives
// `celsius` (temperature - 273), toggles the `eff_steam` component above 50 C, burns or freezes the actor at the
// ends, and spends `freshnessAlpha`; at 0 it takes one from `invRead_freshness` and at none left calls
// `setSpoiled(true)`. A bite takes one from `invRead_uses`. `ignoreRotting` is saved with the rest. Freezing writes
// `tempLessThan0`, calls `freezed(frozen)` and writes custom primitive data 1 on the prop's `staticMesh`.
inline constexpr const wchar_t* FoodClass = L"prop_food_C";
inline constexpr const wchar_t* CookingFoodClass = L"prop_cookingFood_C";
inline constexpr const wchar_t* FoodTemperatureProp = L"temperature";
inline constexpr const wchar_t* FoodCelsiusProp = L"celsius";
inline constexpr const wchar_t* FoodFreshnessProp = L"freshnessAlpha";
inline constexpr const wchar_t* FoodFreshnessLeftProp = L"invRead_freshness";
inline constexpr const wchar_t* FoodUsesProp = L"invRead_uses";
inline constexpr const wchar_t* FoodIgnoreRottingProp = L"ignoreRotting";
inline constexpr const wchar_t* FoodSpoiledProp = L"isSpoiled";
inline constexpr const wchar_t* FoodFrozenProp = L"tempLessThan0";
inline constexpr const wchar_t* FoodSteamComp = L"eff_steam";
inline constexpr const wchar_t* FoodSetSpoiledFn = L"setSpoiled";
inline constexpr const wchar_t* FoodSetSpoiledParam = L"isSpoiled";
inline constexpr const wchar_t* FoodFreezedFn = L"freezed";
inline constexpr const wchar_t* FoodFreezedParam = L"frozen";
inline constexpr const wchar_t* PropStaticMeshComp = L"staticMesh";
inline constexpr const wchar_t* SetCustomPrimitiveDataFloatFn = L"SetCustomPrimitiveDataFloat";
// Freezing's custom primitive data: index 1, 0.5 frozen and 0 not (prop_food_C::ReceiveTick).
inline constexpr int   FoodFrozenDataIndex = 1;
inline constexpr float FoodFrozenDataValue = 0.5f;
inline constexpr float FoodKelvinOffset = 273.0f;
inline constexpr float FoodSteamCelsius = 50.0f;
// prop_cookingFood_C::ReceiveTick adds to `cookProgress` while celsius >= 51 and calls `cookItem` at `cookFinal`.
inline constexpr const wchar_t* FoodCookProgressProp = L"cookProgress";
inline constexpr const wchar_t* FoodCookFinalProp = L"cookFinal";
// The bag roll (prop_garbBagRoll_C): its remaining count is `invRead_bags`, not `bags` -- the roll's
// playerHandUse_LMB subtracts one after each pack, destroys itself at 0 and prints "Bags left: {invRead_bags}".
inline constexpr const wchar_t* GarbBagRollCountProp = L"invRead_bags";
// The non-food `uses` counter some consumables keep instead (prop_snack_C), read where invRead_uses is absent.
inline constexpr const wchar_t* LegacyUsesProp = L"uses";
inline constexpr const wchar_t* NotEdibleProp = L"notEdible";

// The shrimp tray (prop_food_shrimp_C): `shrimps` portions, drawn as instances by `upd()`; action 4 and
// playerHandUse_RMB take one, run upd(), spawn the `shrimpDrop` class 10 cm above the tray and, at none left, spawn
// the prop named shrimp_e in its place and destroy the tray. getData files `shrimps` as ints[0][0].
inline constexpr const wchar_t* ShrimpTrayClass = L"prop_food_shrimp_C";
inline constexpr const wchar_t* ShrimpTrayCountProp = L"shrimps";
inline constexpr const wchar_t* ShrimpTrayDropClassProp = L"shrimpDrop";
inline constexpr const wchar_t* ShrimpTrayUpdFn = L"upd";
inline constexpr int ShrimpTrayMaxCount = 64;   // a sanity bound on the wire, far over the tray's nine

// The microwave (prop_microwave_C; its door is the child actor prop_microwaveDoor_C, a prop_swinger_C). Action 4
// cycles `timeMode` mod 5 and sets `timer` to 5/10/15/30/60 s; action 8 calls `wave()`, which (powered, door shut)
// sets `active`, shows `pointLight`, starts `microwave_loop`, copies `timer` into `time` and runs a latent loop that
// spends `time`, rotates `spin` 60 deg/s and calls microwave(self) / microwaveElec() on each of `objectsInside`;
// at 0 it plays `microwave_ding` and stops. Opening the door while active runs forceOpenDoor (the same stop).
inline constexpr const wchar_t* MicrowaveClass = L"prop_microwave_C";
inline constexpr const wchar_t* MicrowaveDoorClass = L"prop_microwaveDoor_C";
inline constexpr const wchar_t* MicrowaveOpenedProp = L"opened";
inline constexpr const wchar_t* MicrowaveActiveProp = L"active";
inline constexpr const wchar_t* MicrowaveTimerProp = L"timer";
inline constexpr const wchar_t* MicrowaveTimeProp = L"time";
inline constexpr const wchar_t* MicrowaveTimeModeProp = L"timeMode";
inline constexpr const wchar_t* MicrowaveObjectsInsideProp = L"objectsInside";
inline constexpr const wchar_t* MicrowaveDoorObjProp = L"doorObj";
inline constexpr const wchar_t* MicrowaveDelayCloseProp = L"delayClose";
inline constexpr const wchar_t* MicrowaveLoopComp = L"microwave_loop";
inline constexpr const wchar_t* MicrowaveDingComp = L"microwave_ding";
inline constexpr const wchar_t* MicrowaveLightComp = L"pointLight";
inline constexpr const wchar_t* MicrowaveSpinComp = L"spin";
inline constexpr const wchar_t* MicrowaveWaveFn = L"wave";
inline constexpr const wchar_t* CordedPoweredProp = L"cordPowered";
inline constexpr int MicrowaveActionMode = 4;
inline constexpr int MicrowaveActionStart = 8;
inline constexpr int   MicrowaveModeCount = 5;
inline constexpr float MicrowaveSpinDegPerSec = 60.0f;
inline constexpr const wchar_t* AddLocalRotationFn = L"K2_AddLocalRotation";
// The sink and the shower: the dirt scalar `clean` (0 filthy .. 1 spotless) their cleanSponge raises, and the no-arg
// upd() that clamps it and paints it onto the cube's material. The bucket: `height` (the water level), `height_interp`
// (what the look follows), `empty` (the tipped-over / nothing-in-it flag the tap's tick clears) and its no-arg upd().
inline constexpr const wchar_t* SinkClass = L"sink_C";
inline constexpr const wchar_t* ShowerClass = L"prop_shower_C";
// The toilet has the same `clean` and upd(); unlike the other two its own graph also lowers `clean` (use dirties it).
inline constexpr const wchar_t* ToiletClass = L"toilet_C";
inline constexpr const wchar_t* BucketClass = L"prop_bucket_C";
inline constexpr const wchar_t* SurfaceCleanProp = L"clean";
inline constexpr const wchar_t* SurfaceUpdFn = L"upd";
inline constexpr const wchar_t* BucketHeightProp = L"height";
inline constexpr const wchar_t* BucketHeightInterpProp = L"height_interp";
inline constexpr const wchar_t* BucketEmptyProp = L"empty";
inline constexpr const wchar_t* BucketUpdFn = L"upd";

// The basement flood: waterVolume_basementFlooder_C, the one water volume of the basement, which the game moves up and
// down between `loc_a` (dry) and `loc_b` (full, `height` above it). Its level is the actor's distance from loc_a in
// world units: getLevel reads it, setLevel(alpha) places the actor there in one step, and addWater(deltaTime, power)
// walks it toward loc_b (power > 0, a burst pipe's tick) or loc_a (power < 0, the drain's tick). startMoving(moveTo)
// is the cheat's toggle (`bflood`): it sets `moveTo` and enables the actor's tick, whose body refreshes the water's
// data through the parent waterVolume_C's no-arg updateWaterData (surface plane, depth, the buoyancy boxes).
inline constexpr const wchar_t* BasementFloodClass = L"waterVolume_basementFlooder_C";
inline constexpr const wchar_t* BasementFloodGetLevelFn = L"getLevel";
inline constexpr const wchar_t* BasementFloodSetLevelFn = L"setLevel";
inline constexpr const wchar_t* BasementFloodAddWaterFn = L"addWater";
inline constexpr const wchar_t* BasementFloodStartMovingFn = L"startMoving";
inline constexpr const wchar_t* BasementFloodMoveToProp = L"moveTo";
inline constexpr const wchar_t* BasementFloodHeightProp = L"height";
// `dist` is set by the actor's BeginPlay (the distance between loc_a and loc_b), so it is zero until the actor has begun
// play, and loc_a is then still the origin: a level read or placed before that is meaningless.
inline constexpr const wchar_t* BasementFloodDistProp = L"dist";
inline constexpr const wchar_t* BasementFloodLevelParam = L"level";
inline constexpr const wchar_t* BasementFloodAlphaParam = L"alpha";
inline constexpr const wchar_t* WaterVolumeUpdateDataFn = L"updateWaterData";
// The anti-noclip check of mainPlayer_C's tick (ue_wrap/actors/anti_noclip.h): `lastLoc`, the camera's place at the end
// of the last tick; the gamemode's canBackrooms() and its out parameter; the controller's GetViewTarget; and
// teleportWOBackrooms (newTransform, useRotation, trueRotation, useVelocity, controlRotation), the verb both branches
// end in, which also writes `lastLoc` and takes the velocity from the transform's scale when useVelocity is set.
inline constexpr const wchar_t* MainPlayerLastLocProp = L"lastLoc";
inline constexpr const wchar_t* MainPlayerTeleportFn = L"teleportWOBackrooms";
inline constexpr const wchar_t* TeleportNewTransformParam = L"newTransform";
inline constexpr const wchar_t* TeleportUseRotationParam = L"useRotation";
inline constexpr const wchar_t* TeleportTrueRotationParam = L"trueRotation";
inline constexpr const wchar_t* TeleportUseVelocityParam = L"useVelocity";
inline constexpr const wchar_t* TeleportControlRotationParam = L"controlRotation";
inline constexpr const wchar_t* CanBackroomsReturnParam = L"return";
inline constexpr const wchar_t* GetViewTargetFn = L"GetViewTarget";
// The collision channel the anti-noclip check reads the capsule's response on (ECollisionChannel 16, game channel 10).
inline constexpr int AntiNoclipCapsuleChannel = 16;

// The basement's burst water pipes (burstWaterPipe_C) and the toolbox that repairs them. A pipe holds `damaged`; its
// state(damaged) sets the flag and the tick, the spray and the sound; addDamage(...) is state(true); and its toolbox
// interface answers toolboxCanFix (damaged), toolboxFixTime (3 s) and toolboxFix(toolbox), which is state(false) and
// true only while damaged. While damaged its tick calls the flooder's addWater(dt, 1). The kit (prop_toolbox_C), held,
// takes the looked-at actor as `fixingActor` on playerHandUse_LMB when that actor can be fixed, reads its fix time into
// `time` (a prop_powertool_C in the offhand divides it by 5), raises `complete` on its tick, and at 1 calls the target's
// toolboxFix and, when it returns true, takes one of `fixes` and ends (completeSaw, usesMessage, a destroy when
// `destroyOnUse` and none are left). `fixAudio` is its repair sound, played every 0.8 s while the aim holds. The drain
// (basementDrain_C) holds `blockage`, the share of its sample points a prop clogs.
inline constexpr const wchar_t* BurstPipeClass = L"burstWaterPipe_C";
inline constexpr const wchar_t* BurstPipeStateFn = L"state";
inline constexpr const wchar_t* BurstPipeStateParam = L"damaged";
inline constexpr const wchar_t* BurstPipeAddDamageFn = L"addDamage";
inline constexpr const wchar_t* BurstPipeDamagedProp = L"damaged";
inline constexpr const wchar_t* BurstPipeFixFn = L"toolboxFix";
inline constexpr const wchar_t* BurstPipeFixTimeFn = L"toolboxFixTime";
inline constexpr const wchar_t* BurstPipeToolboxParam = L"toolbox";
inline constexpr const wchar_t* ToolboxClass = L"prop_toolbox_C";
inline constexpr const wchar_t* PowerToolClass = L"prop_powertool_C";
inline constexpr const wchar_t* ToolboxFixingActorProp = L"fixingActor";
inline constexpr const wchar_t* ToolboxFixesProp = L"fixes";
inline constexpr const wchar_t* ToolboxTimeProp = L"time";
inline constexpr const wchar_t* ToolboxCompleteProp = L"complete";
inline constexpr const wchar_t* ToolboxFixAudioProp = L"fixAudio";
inline constexpr const wchar_t* ToolboxCompleteSawFn = L"completeSaw";
inline constexpr float ToolboxFixSoundPeriodSec = 0.8f;
inline constexpr float ToolboxPowerToolDivisor = 5.0f;
inline constexpr const wchar_t* BasementDrainClass = L"basementDrain_C";
inline constexpr const wchar_t* BasementDrainBlockageProp = L"blockage";

// The buoyancy master (waterFloatMaster_C): one per world, holding in `actorsInWater` every body a water volume has
// reported inside it, which its tick pushes toward the surface.
inline constexpr const wchar_t* WaterFloatMasterClass = L"waterFloatMaster_C";
inline constexpr const wchar_t* WaterFloatMasterActorsProp = L"actorsInWater";

// The poster, in its two forms. Rolled: prop_poster_C, a prop held in the hand; its left-button use calls put(),
// which (the holder's hitResult blocking, on a body that does not simulate) spawns `posterClass` at the `turn`
// component's world transform (scale included: the scroll wheel sizes `spawnLoc`, `turn`'s parent), copies `type`,
// `fileName` and `custom` into it in the deferred window and destroys itself. Its getData writes ints[0] =
// {type, 0} and strings[1] = {fileName}; its loadData restores both and does NOT redraw, which setTex() does
// (`custom`: the texture of gamemode.filenames_posters' match for `fileName`, else lib_C::getPosters(type)).
// Placed: poster_C, an actor_save_C (not a prop) with a decal; its getData writes ints[0] = {type, index} and
// strings[1] = {fileName}, its loadData restores them without a redraw, which setPoster() does (BeginPlay runs it
// a frame late). Its action 7 spawns `posterClass` (the rolled form) at the player with type, index and fileName,
// hands it to the player (pickupObjectDirect) and destroys itself; its action 4 on a `custom` poster opens the
// texture picker, whose intComs_texturePickerApply writes `fileName` and redraws (the rolled form's does too).
// `custom` is a class default of the rolled form, saved by neither record. A missing custom file is null-safe in
// the Blueprint: the decal goes flat.
inline constexpr const wchar_t* PosterRolledClass = L"prop_poster_C";
inline constexpr const wchar_t* PosterPlacedClass = L"poster_C";
inline constexpr const wchar_t* PosterTypeProp = L"type";
inline constexpr const wchar_t* PosterIndexProp = L"index";
inline constexpr const wchar_t* PosterFileNameProp = L"fileName";
inline constexpr const wchar_t* PosterCustomProp = L"custom";
inline constexpr const wchar_t* PosterClassProp = L"posterClass";
inline constexpr const wchar_t* PosterTurnComp = L"turn";
inline constexpr const wchar_t* PosterHoldingPlayerProp = L"holdingPlayer";
inline constexpr const wchar_t* PosterGamemodeProp = L"gamemode";
inline constexpr const wchar_t* PosterCustomFilesProp = L"filenames_posters";   // on mainGamemode_C
inline constexpr const wchar_t* PosterRolledRedrawFn = L"setTex";
inline constexpr const wchar_t* PosterPlacedRedrawFn = L"setPoster";
inline constexpr const wchar_t* PosterPutFn = L"put";
inline constexpr const wchar_t* PosterPickerApplyFn = L"intComs_texturePickerApply";
inline constexpr const wchar_t* PosterPickedFileNameParam = L"pickedFileName";
inline constexpr const wchar_t* ComponentScaleFn = L"K2_GetComponentScale";
inline constexpr int PosterActionTakeDown = 7;
inline constexpr int PosterActionPicker = 4;
// The crowbar's pry. A held prop_crowbar_C's playerHandUse_RMB runs activatePrying, which finds the hidden
// pryingCrowbar_C child actor of the looked-at pryable and runs its begin(mainPlayer, out failed): the held crowbar is
// destroyed, its `lives` copied to `currentLives`, the child revealed with `sk_pivot` at the hit and a skeletal
// `crowbarSK` built under it (createSkeletalMesh) with an AnimBP whose `angle` bends it. The player's LMB on the
// stuck crowbar (playerUsedOn) binds its mouse to mouseXY, which raises `alpha` and `snapStress`; updPivot pitches
// `sk_pivot` and writes the AnimBP's `angle`; `strainSound` alternates the two strain audio components and `sparked`
// lights `eff_spark`. end(pryOpen, dropSelf, destroyed) gives the crowbar back (flung on a pry, dropped, or into the
// hotbar of `lastPlayer`; none when destroyed), hides the child and, on a pry, runs the parent's crowbarOpen(self).
// snap() is the strain's break: one life less, a new crowbar or its gibs, then end(false, false, true).
inline constexpr const wchar_t* PryingCrowbarClass = L"pryingCrowbar_C";
inline constexpr const wchar_t* PryBeginFn = L"begin";
inline constexpr const wchar_t* PryEndFn = L"end";
inline constexpr const wchar_t* PrySnapFn = L"snap";
inline constexpr const wchar_t* PryCreateSkeletalMeshFn = L"createSkeletalMesh";
inline constexpr const wchar_t* PryPivotComp = L"sk_pivot";
inline constexpr const wchar_t* PrySkeletalProp = L"crowbarSK";
inline constexpr const wchar_t* PryAnimProp = L"anim";
inline constexpr const wchar_t* PryAnimAngleProp = L"angle";
inline constexpr const wchar_t* PrySparkComp = L"eff_spark";
inline constexpr const wchar_t* PryStrainAudioA = L"audio_strain1";
inline constexpr const wchar_t* PryStrainAudioB = L"audio_strain2";
inline constexpr const wchar_t* PryUiProp = L"pryingCrowbarUI";
inline constexpr const wchar_t* PryLastPlayerProp = L"lastPlayer";
inline constexpr const wchar_t* PryAlphaProp = L"alpha";
inline constexpr const wchar_t* PryStressProp = L"snapStress";
inline constexpr const wchar_t* PryLivesProp = L"currentLives";
inline constexpr const wchar_t* PryStrainSoundProp = L"strainSound";
inline constexpr const wchar_t* PryStrainingProp = L"straining";
inline constexpr const wchar_t* PrySparkedProp = L"sparked";
inline constexpr const wchar_t* PryStickCue = L"stick_Cue";
inline constexpr const wchar_t* CrowbarClass = L"prop_crowbar_C";
// The prop_asologoPiece_C family (radiator_C, radiator_base2_C, securityTV_C), the pryables whose crowbarOpen runs the
// inherited breakingSequence. Its `break()` is the cosmetic half alone: it activates the break particles (eff_break)
// and the tear-off sound (audio_offthewall); the sequence that calls it also frees the piece's physics and breaks its
// four world constraints one by one.
inline constexpr const wchar_t* AsologoPieceClass = L"prop_asologoPiece_C";
inline constexpr const wchar_t* AsologoPieceBreakFn = L"break";
inline constexpr int CrowbarMaxLives = 3;
// The hacksaw (prop_hacksaw_C). Its playerHandUse_LMB takes the looked-at heavy simulating prop of a scrappable
// material as `sawingProp`: processMat() names the scrap in `drop`, `vol` is the mesh's convex volume
// (BPCodeLib::MeshConvexVolume2), `time` = vol / 100000 / 2 * the physical material's `Density`, `amount` =
// max(round(vol / 100000 / 2), 1). ReceiveTick adds delta / time to `complete`; at 1 it spawns `amount` scraps in the
// prop's bounds, runs its broken() and destroys it, then completeSaw(). completeSaw() is the reset (release, a lost
// aim, the saw's destroy); a 0.8 s timer runs sawSound, which plays sawsaw_Cue in 2D while the aim holds.
inline constexpr const wchar_t* HacksawClass = L"prop_hacksaw_C";
inline constexpr const wchar_t* HacksawSawingProp = L"sawingProp";
inline constexpr const wchar_t* HacksawCompleteProp = L"complete";
inline constexpr const wchar_t* HacksawTimeProp = L"time";
inline constexpr const wchar_t* HacksawAmountProp = L"amount";
inline constexpr const wchar_t* HacksawCompleteSawFn = L"completeSaw";
inline constexpr const wchar_t* HacksawProcessMatFn = L"processMat";
inline constexpr const wchar_t* HacksawSawSoundFn = L"sawSound";
inline constexpr const wchar_t* HandUseLmbFn = L"playerHandUse_LMB";
inline constexpr const wchar_t* ReceiveTickFn = L"ReceiveTick";
inline constexpr const wchar_t* IsNotSawableFn = L"isNotSawable";
inline constexpr const wchar_t* SawSoundCue = L"sawsaw_Cue";
inline constexpr const wchar_t* MeshVolumeLibClass = L"BPCodeLib";
inline constexpr const wchar_t* MeshVolumeFn = L"MeshConvexVolume2";
inline constexpr float HacksawVolumeDivisor = 200000.0f;  // vol / 100000 / 2
inline constexpr float HacksawSoundPeriodSec = 0.8f;

// The two completion stores (ue_wrap/world/achievements). The profile (save_main_C) keeps the achievements, the
// world's save slot (saveSlot_C) the advancements, each as three parallel arrays: the row names, a struct_achievement
// per row (progress, progressTarget, award) and a completed flag; a row is appended when it first progresses.
// Their catalogues are the cooked list_achievements and list_advancements data tables.
inline constexpr const wchar_t* AchievementCatalogue = L"list_achievements";
inline constexpr const wchar_t* AdvancementCatalogue = L"list_advancements";
inline constexpr const wchar_t* AchievementNamesProp = L"achievementsNames";
inline constexpr const wchar_t* AchievementRowsProp = L"achievements";
inline constexpr const wchar_t* AchievementCompletedProp = L"achievementsCompleted";
inline constexpr const wchar_t* AdvancementNamesProp = L"advancementsNames";
inline constexpr const wchar_t* AdvancementRowsProp = L"advancements";
inline constexpr const wchar_t* AdvancementCompletedProp = L"advancementsCompleted";
inline constexpr const wchar_t* AchievementProgressMember = L"progress_";
inline constexpr const wchar_t* AchievementTargetMember = L"progressTarget_";
inline constexpr const wchar_t* ProgressAdvancementFn = L"progressAdvancement";

// The cleaning brushes. Every cleaner of the bay window is a prop_sponge_C: the held sponge, the mop
// (prop_mop_C), the bucket pour (prop_sponge_bucketpour_C) and the pressure washer's water shot
// (prop_sponge_p_C, a short-lived projectile prop_waterShooter_C fires from its tick). Each makes its brush
// material in BeginPlay with itself as the outer, from its class default's `drawmat`, so a dab's material
// names the cleaner that drew it.
inline constexpr const wchar_t* SpongeClass = L"prop_sponge_C";
inline constexpr const wchar_t* MopClass = L"prop_mop_C";
inline constexpr const wchar_t* SpongePourClass = L"prop_sponge_bucketpour_C";
inline constexpr const wchar_t* SpongeShotClass = L"prop_sponge_p_C";
inline constexpr const wchar_t* SpongeDrawMatProp = L"drawmat";

// The pressure washer, prop_waterShooter_C. playerHandUse_LMB sets `fire` and enables the actor's tick;
// release clears both. Each tick with `fire` set and `level` (the tank, litres) above zero drains the tank,
// fires one prop_sponge_p_C from the player's camera along its forward (x5000 plus a random 100 spread)
// and plays wade_Cue in 2D. The shot spawns eff_waterThrower where it is born; where it hits it plays
// wade_Cue and spawns eff_waterThrowSplash, then cleans what it overlaps.
inline constexpr const wchar_t* WaterShooterClass = L"prop_waterShooter_C";
inline constexpr const wchar_t* WaterShooterFireProp = L"fire";
inline constexpr const wchar_t* WaterShooterLevelProp = L"level";
inline constexpr const wchar_t* WasherSprayFx = L"eff_waterThrower";
inline constexpr const wchar_t* WasherSplashFx = L"eff_waterThrowSplash";
inline constexpr const wchar_t* WasherCue = L"wade_Cue";
inline constexpr const wchar_t* ParticleSystemClass = L"ParticleSystem";
inline constexpr const wchar_t* SoundCueClass = L"SoundCue";
}  // namespace name

}  // namespace ue_wrap::profile
