enum class EAIAudioState {
    Undefined = 0,
    Idle = 1,
    Fleeing = 2,
    Attacking = 3,
    Dead = 4,
    Relaxing = 5,
    Mounted = 6,
    Stalking = 7,
    EAIAudioState_MAX = 8,
};

enum EAIEventRequestResponse {
    Invalid = 0,
    FailedToStart = 1,
    EventStarted = 2,
    PendingLoad = 3,
    EAIEventRequestResponse_MAX = 4,
};

enum class EAIVocalisationType {
    Attack = 0,
    Flinch = 1,
    Death = 2,
    EAIVocalisationType_MAX = 3,
};

enum class EActionRangeCheckBehaviour {
    ValidMove = 0,
    CustomFunction = 1,
    Both = 2,
    EActionRangeCheckBehaviour_MAX = 3,
};

enum class EActionableEventType {
    Undefined = 0,
    Primary = 1,
    Secondary = 2,
    Tertiary = 3,
    Reload = 4,
    MAX_VALUE = 5,
    EActionableEventType_MAX = 6,
};

enum class EActionableTrigger {
    ActionPressed = 0,
    ActionReleased = 1,
    ActionHeld = 2,
    EActionableTrigger_MAX = 3,
};

enum class EAliveState {
    Alive = 0,
    Dead = 1,
    EAliveState_MAX = 2,
};

enum class EAnimOverlayState {
    Default = 0,
    OneHanded = 1,
    Bow = 2,
    TwoHandedRifle = 3,
    Driving = 4,
    Spear = 5,
    Carrying = 6,
    Firearm = 7,
    Fishing = 8,
    EAnimOverlayState_MAX = 9,
};

enum class EAnimStateFMODParam {
    NotAnimating = 0,
    Animating = 1,
    EAnimStateFMODParam_MAX = 2,
};

enum class EAntiAliasingSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EAntiAliasingSetting_MAX = 256,
};

enum class EArmourType {
    Undefined = 0,
    Head = 1,
    Chest = 2,
    Hands = 3,
    Legs = 4,
    Feet = 5,
    Undersuit = 6,
    Skin_Head = 7,
    Undersuit_Helmet = 8,
    Skin_Head_Hair = 9,
    Backpack = 10,
    Gauntlet = 11,
    EArmourType_MAX = 12,
};

enum class EAssetType {
    Object = 0,
    Class = 1,
    EAssetType_MAX = 2,
};

enum class EAudioOcclusionMode {
    Complex = 0,
    SourceSimple = 1,
    SourceAndListenerSimple = 2,
    EAudioOcclusionMode_MAX = 3,
};

enum class EAudioShelterState {
    Low = 0,
    Medium = 1,
    High = 2,
    EAudioShelterState_MAX = 3,
};

enum class EAuthorityType {
    ClientOnly = 0,
    ServerOnly = 1,
    Both = 2,
    EAuthorityType_MAX = 3,
};

enum class EBestiaryUnlockPopup {
    Creature = 0,
    Stat1 = 1,
    Stat2 = 2,
    Lore1 = 3,
    Lore2 = 4,
    Lore3 = 5,
    Weaknesses = 6,
    Loot = 7,
    EBestiaryUnlockPopup_MAX = 8,
};

enum class EBiomeImageType {
    None = 0,
    Small = 1,
    Medium = 2,
    Large = 3,
    EBiomeImageType_MAX = 4,
};

enum class EBuildingDestroyReason {
    Stability = 0,
    Player = 1,
    Damaged = 2,
    Replaced = 3,
    EBuildingDestroyReason_MAX = 4,
};

enum class EBuildingMeshType {
    Invalid = 0,
    BaseMesh = 1,
    FrameMesh = 2,
    EBuildingMeshType_MAX = 3,
};

enum class EBuildingOpenFMODParam {
    Closed = 0,
    Open = 1,
    EBuildingOpenFMODParam_MAX = 2,
};

enum class EBuildingPieceType {
    Floor = 0,
    Wall = 1,
    Frame = 2,
    Ramp = 3,
    EBuildingPieceType_MAX = 4,
};

enum class EBuildingUnzipFMODParam {
    Normal = 0,
    Unzipping = 1,
    EBuildingUnzipFMODParam_MAX = 2,
};

enum class ECanHitResult {
    CantHit = 0,
    Miss = 1,
    Hit = 2,
    ECanHitResult_MAX = 3,
};

enum class ECanRepair {
    HaveIngredients = 0,
    RequiresIngredients = 1,
    NeedsPower = 2,
    ToResolve = 3,
    ECanRepair_MAX = 4,
};

enum class ECanUseItemResult {
    VisibleAndEnabled = 0,
    VisibleAndDisabled = 1,
    Hidden = 2,
    ECanUseItemResult_MAX = 3,
};

enum class ECaveContextFMODParam {
    None = 0,
    ListenerOutSourceOut = 1,
    ListenerOutSourceInCave = 2,
    ListenerInCaveSourceOut = 3,
    ListenerInCaveSourceInCave = 4,
    ECaveContextFMODParam_MAX = 5,
};

enum class ECaveLightType {
    Spot = 0,
    Rect = 1,
    ECaveLightType_MAX = 2,
};

enum class EChallengeTypes {
    KillCreature = 0,
    CriticalHit = 1,
    StealthAttack = 2,
    SkinCreature = 3,
    HarvestCreatureItem = 4,
    HarvestPlant = 5,
    FellTree = 6,
    CollectTreeItem = 7,
    FullyMineVoxel = 8,
    MineResource = 9,
    EChallengeTypes_MAX = 10,
};

enum class ECharacterBodyType {
    Masculine = 0,
    Feminine = 1,
    Neutral = 2,
    ECharacterBodyType_MAX = 3,
};

enum class ECharacterCustomisationContext {
    Undefined = 0,
    CharacterCreation = 1,
    HABCustomisation = 2,
    ECharacterCustomisationContext_MAX = 3,
};

enum class ECharacterOptionCategory {
    Head = 0,
    Body = 1,
    BodyColor = 2,
    HairStyle = 3,
    HairColor = 4,
    Head_Tattoo = 5,
    Head_Scar = 6,
    Head_FacialHair = 7,
    SkinTone = 8,
    Color = 9,
    EyeColor = 10,
    Decal = 11,
    ECharacterOptionCategory_MAX = 12,
};

enum class ECheatsEnabled {
    Enabled = 0,
    NotEnabled = 1,
    ECheatsEnabled_MAX = 2,
};

enum class EClassRepPolicy {
    NotRouted = 0,
    ManuallyRouted = 1,
    RelevantAllConnections = 2,
    Spatialize_Static = 3,
    Spatialize_Dynamic = 4,
    Spatialize_Dormancy = 5,
    EClassRepPolicy_MAX = 6,
};

enum class ECombinedCaveComponentFlags {
    None = 0,
    EntranceComponent = 1,
    Void = 2,
    ECombinedCaveComponentFlags_MAX = 3,
};

enum class EComparisonType {
    Equals = 0,
    NotEquals = 1,
    LessThan = 2,
    LessThanOrEqual = 3,
    GreaterThan = 4,
    GreaterThanOrEqual = 5,
    EComparisonType_MAX = 6,
};

enum class EControllerIconSet {
    None = 0,
    Xbox = 1,
    Playstation = 2,
    NintendoSwitch = 3,
    EControllerIconSet_MAX = 4,
};

enum class EControllerIconsSetting {
    Xbox = 0,
    Playstation = 1,
    Switch = 2,
    NumSettings = 3,
    Custom = 255,
    EControllerIconsSetting_MAX = 256,
};

enum class ECreatureAudioThreatTargetType {
    Invalid = 0,
    OtherPlayer = 1,
    LocalPlayer = 2,
    OtherCreature = 3,
    Stimulus = 4,
    ECreatureAudioThreatTargetType_MAX = 5,
};

enum class ECreatureFoliageFMODParam {
    NotInFoliage = 0,
    InFoliage = 1,
    ECreatureFoliageFMODParam_MAX = 2,
};

enum class ECreatureFootstepTypeFMODParam {
    FrontFoot = 0,
    RearFoot = 1,
    Jump = 2,
    JumpLand = 3,
    ECreatureFootstepTypeFMODParam_MAX = 4,
};

enum class ECreatureSex {
    Unknown = 0,
    Female = 1,
    Male = 2,
    ECreatureSex_MAX = 3,
};

enum class ECropMeshRotationType {
    NoRotation = 0,
    Random90 = 1,
    FullyRandom = 2,
    ECropMeshRotationType_MAX = 3,
};

enum class ECrosshairColorSetting {
    White = 0,
    Red = 1,
    Green = 2,
    Blue = 3,
    Yellow = 4,
    Pink = 5,
    Cyan = 6,
    Black = 7,
    NumSettings = 8,
    Custom = 255,
    ECrosshairColorSetting_MAX = 256,
};

enum class ECrosshairStyleSetting {
    Dot = 0,
    Chevron = 1,
    Circle = 2,
    Cross = 3,
    Plus = 4,
    Diamond = 5,
    Notched = 6,
    Scope = 7,
    Bullseye = 8,
    Spread = 9,
    Target = 10,
    NumSettings = 11,
    Custom = 255,
    ECrosshairStyleSetting_MAX = 256,
};

enum class ECustomGameStatCategory {
    Player = 0,
    Weather = 1,
    Creatures = 2,
    Misc = 3,
    ECustomGameStatCategory_MAX = 4,
};

enum class ECustomGameStatChangeability {
    OnProspectCreation = 0,
    OnProspectLoad = 1,
    Anytime = 2,
    ECustomGameStatChangeability_MAX = 3,
};

enum class ECustomGameStatType {
    Bool = 0,
    Int = 1,
    DropDown = 2,
    ECustomGameStatType_MAX = 3,
};

enum class EDamageTypeFMODParam {
    Undefined = 0,
    Pure = 1,
    Physical = 2,
    Melee = 3,
    Ranged = 4,
    Fire = 5,
    FallDamage = 6,
    Collision = 7,
    Poison = 8,
    Wind = 9,
    EDamageTypeFMODParam_MAX = 10,
};

enum class EDataValid {
    DataValid = 0,
    DataInvalid = 1,
    EDataValid_MAX = 2,
};

enum class EDataValidity {
    Valid = 0,
    Invalid = 1,
    EDataValidity_MAX = 2,
};

enum class EDeployableSnapBehaviour {
    WorldPlacementOnly = 0,
    SnapPlacementOnly = 1,
    WorldAndSnap = 2,
    EDeployableSnapBehaviour_MAX = 3,
};

enum class EDestroyPattern {
    RadialOutward = 0,
    RadialInward = 1,
    Random = 2,
    EDestroyPattern_MAX = 3,
};

enum class EDeviceState {
    On = 0,
    Idle = 1,
    Off = 2,
    EDeviceState_MAX = 3,
};

enum class EDialogueEvents {
    None = 0,
    QuestStart = 1,
    QuestEnd = 2,
    EDialogueEvents_MAX = 3,
};

enum class EDialogueRedirectCondition {
    None = 0,
    IsOpenWorldProspect = 1,
    IsMissionProspect = 2,
    EDialogueRedirectCondition_MAX = 3,
};

enum class EDirtierMode {
    OwningActor = 0,
    AffectedObjectsList = 1,
    EDirtierMode_MAX = 2,
};

enum class EDisplayMode {
    Fullscreen = 0,
    Borderless = 1,
    Windowed = 2,
    EDisplayMode_MAX = 3,
};

enum class EDisplayTemperatureSetting {
    Celsius = 0,
    Fahrenheit = 1,
    NumSettings = 2,
    Custom = 255,
    EDisplayTemperatureSetting_MAX = 256,
};

enum class EDropAbundance {
    Low = 0,
    Medium = 1,
    High = 2,
    EDropAbundance_MAX = 3,
};

enum class EDropTemperature {
    Cold = 0,
    Normal = 1,
    Hot = 2,
    EDropTemperature_MAX = 3,
};

enum class EDropshipDescentStateFMODParam {
    MainEngines = 0,
    Booster = 1,
    Freefall = 2,
    Landed = 3,
    EnterSeat = 4,
    CrashBegin = 5,
    CrashEnd = 6,
    EDropshipDescentStateFMODParam_MAX = 7,
};

enum class EDynamicItemProperties {
    AssociatedItemInventoryId = 0,
    AssociatedItemInventorySlot = 1,
    DynamicState = 2,
    GunCurrentMagSize = 3,
    CurrentAmmoType = 4,
    BuildingVariation = 5,
    Durability = 6,
    ItemableStack = 7,
    MillijoulesRemaining = 8,
    TransmutableUnits = 9,
    Fillable_StoredUnits = 10,
    Fillable_Type = 11,
    Decayable_CurrentSpoilTime = 12,
    InventoryContainer_LinkedInventoryId = 13,
    MaxDynamicItemProperties = 14,
    EDynamicItemProperties_MAX = 15,
};

enum class EDynamicQuestDifficulty {
    None = 0,
    Easy = 1,
    Medium = 2,
    Hard = 3,
    EDynamicQuestDifficulty_MAX = 4,
};

enum class EEffectsSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EEffectsSetting_MAX = 256,
};

enum class EEndProspectSessionContext {
    Undefined = 0,
    HostLeavingSession = 1,
    ExpiredProspect = 2,
    Error_InvalidHost = 3,
    Error_FailedToHostSession = 4,
    EEndProspectSessionContext_MAX = 5,
};

enum class EEnvironmentLightningTargetFMODParam {
    Random = 0,
    Player = 1,
    Tree = 2,
    Building = 3,
    EEnvironmentLightningTargetFMODParam_MAX = 4,
};

enum class EErrorAction {
    Immediate = 0,
    Kick = 1,
    Queue = 2,
    AppClose = 3,
    EErrorAction_MAX = 4,
};

enum class EErrorTarget {
    Log = 0,
    Widget = 1,
    Dialog = 2,
    EErrorTarget_MAX = 3,
};

enum class EEventEndReason {
    InvalidReason = 0,
    Completed = 1,
    Timeout = 2,
    Aborted = 3,
    EEventEndReason_MAX = 4,
};

enum class EExperienceSource {
    XP_None = 0,
    XP_OnAction = 1,
    XP_OnInteract = 2,
    XP_OnHit = 3,
    XP_OnDamaged = 4,
    XP_OnDeath = 5,
    XP_OnCraft = 6,
    XP_OnAchievement = 7,
    XP_Misc = 8,
    XP_MAX = 9,
};

enum class EFLODActorState {
    Undefined = 0,
    Revealing = 2,
    Revealed = 4,
    Concealing = 8,
    Concealed = 16,
    EFLODActorState_MAX = 17,
};

enum class EFLODLevelInfluenceType {
    None = 0,
    ViewTrace = 1,
    Distance = 2,
    EFLODLevelInfluenceType_MAX = 3,
};

enum class EFSRModeSetting {
    Off = 0,
    Performance = 1,
    Balanced = 2,
    Quality = 3,
    Ultra_Quality = 4,
    NumSettings = 5,
    Custom = 255,
    EFSRModeSetting_MAX = 256,
};

enum class EFeatureLevelCheckResult {
    Unchecked = 0,
    Fail_FeatureLevel = 1,
    Fail_Flag = 2,
    Unknown_CouldNotCheckFlag = 3,
    Pass = 4,
    EFeatureLevelCheckResult_MAX = 5,
};

enum class EFieldGuideItemHotToObtain {
    Unobtainium = 0,
    Harvest = 1,
    Craft = 2,
    Kill = 4,
    Fishing = 8,
    PickAxe = 16,
    SledgeHammer = 32,
    DrillOrExtract = 64,
    Buy_At_Workshop = 128,
    EFieldGuideItemHotToObtain_MAX = 129,
};

enum class EFireExtinguishResult {
    Failed = 0,
    ExtinguishedCombustion = 1,
    ExtinguishedPyrolysis = 2,
    EFireExtinguishResult_MAX = 3,
};

enum class EFireMode {
    Semiauto = 0,
    Burst = 1,
    Auto = 2,
    EFireMode_MAX = 3,
};

enum class EFireStateFMODParam {
    NotOnFire = 0,
    OnFire = 1,
    EFireStateFMODParam_MAX = 2,
};

enum class EFirearmAttachType {
    Weapon = 0,
    Player = 1,
    EFirearmAttachType_MAX = 2,
};

enum class EFishRarity {
    None = 0,
    Common = 1,
    Uncommon = 2,
    Rare = 3,
    Unique = 4,
    EFishRarity_MAX = 5,
};

enum class EFishType {
    None = 0,
    Saltwater = 1,
    Freshwater = 2,
    EFishType_MAX = 3,
};

enum class EFishUnlockPopup {
    None = 0,
    FishCaught = 1,
    Quality = 2,
    Weight = 4,
    Length = 8,
    All = 255,
    EFishUnlockPopup_MAX = 256,
};

enum class EFlagsTableType {
    D_CharacterFlags = 0,
    D_SessionFlags = 1,
    D_AccountFlags = 2,
    D_DLCPackageData = 3,
    None = 255,
    EFlagsTableType_MAX = 256,
};

enum class EFlammableAudioLocationType {
    ActorLocation = 0,
    BoundsOrigin = 1,
    BoundsBase = 2,
    EFlammableAudioLocationType_MAX = 3,
};

enum class EFlammablePropagationType {
    None = 0,
    Self = 1,
    FireInstance = 2,
    EFlammablePropagationType_MAX = 3,
};

enum class EFlammableState {
    None = 0,
    Detached = 1,
    Pyrolysis = 2,
    Combusting = 3,
    Combusted = 4,
    Destroyed = 5,
    EFlammableState_MAX = 6,
};

enum class EFloatRoundingMode {
    Round = 0,
    Floor = 1,
    Ceiling = 2,
    EFloatRoundingMode_MAX = 3,
};

enum class EFoliageSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EFoliageSetting_MAX = 256,
};

enum class EForceRemovePlayerReason {
    Initialisation_NotApproved = 0,
    KickedByHostingPlayer = 1,
    EForceRemovePlayerReason_MAX = 2,
};

enum class EFound {
    Found = 0,
    NotFound = 1,
    EFound_MAX = 2,
};

enum class EFunctionOutcome {
    Success = 0,
    Failure = 1,
    EFunctionOutcome_MAX = 2,
};

enum class EGOAPCharacterStance {
    Standing = 0,
    Sitting = 1,
    Lying = 2,
    EGOAPCharacterStance_MAX = 3,
};

enum class EGOAPControllerState {
    Idle = 0,
    GetNewAction = 1,
    MoveToAction = 2,
    PerformAction = 3,
    EGOAPControllerState_MAX = 4,
};

enum class EGOAPFactSource {
    VisionPerception = 0,
    SoundPerception = 1,
    DamagePerception = 2,
    ProtectiveMotivation = 3,
    EGOAPFactSource_MAX = 4,
};

enum class EGOAPObjectType {
    Food = 0,
    Water = 1,
    Enemy = 2,
    MaxObjectTypes = 3,
    EGOAPObjectType_MAX = 4,
};

enum class EGOAPProperty {
    Hungry = 0,
    Thirsty = 1,
    HasFood = 2,
    HasWater = 3,
    FoundFood = 4,
    FoundWater = 5,
    Wander = 6,
    Scared = 7,
    RunForSafety = 8,
    MaxProperties = 9,
    EGOAPProperty_MAX = 10,
};

enum class EGenderedArmourType {
    Invalid = 0,
    MaleThirdPerson = 1,
    FemaleThirdPerson = 2,
    GenericFirstPerson = 3,
    EGenderedArmourType_MAX = 4,
};

enum class EGlobalDropStateFMODParam {
    Hab = 0,
    Dropship = 1,
    Prospect = 2,
    LoadingProspect = 3,
    EGlobalDropStateFMODParam_MAX = 4,
};

enum class EGlobalEnvironmentBiomeFMODParam {
    None = 0,
    Conifer = 1,
    Arctic = 2,
    Desert = 3,
    Lava = 4,
    Wetlands = 5,
    Grasslands = 6,
    EGlobalEnvironmentBiomeFMODParam_MAX = 7,
};

enum class EGlobalEnvironmentTerrainZoneFMODParam {
    Default = 0,
    Canyon_Narrow = 1,
    Canyon_Med = 2,
    Canyon_Wide = 3,
    EGlobalEnvironmentTerrainZoneFMODParam_MAX = 4,
};

enum class EGlobalLoadingScreenStateFMODParam {
    LoadingScreen_Inactive = 0,
    LoadingScreen_Active = 1,
    LoadingScreen_MAX = 2,
};

enum class EGlobalPlayerCharacterVoiceFMODParam {
    None = 0,
    VoiceA = 1,
    VoiceB = 2,
    EGlobalPlayerCharacterVoiceFMODParam_MAX = 3,
};

enum class EGraphicsCardVendor {
    Invalid = 0,
    Unknown = 1,
    Nvidia = 2,
    AMD = 3,
    Intel = 4,
    EGraphicsCardVendor_MAX = 5,
};

enum class EGreatHuntMissionType {
    None = 0,
    Standard = 1,
    Choice = 2,
    Optional = 3,
    Final = 4,
    EGreatHuntMissionType_MAX = 5,
};

enum class EHandedness {
    Right = 0,
    Left = 1,
    Both = 2,
    EHandedness_MAX = 3,
};

enum class EHeatmapColorChannel {
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3,
    AnyChannel = 4,
    EHeatmapColorChannel_MAX = 5,
};

enum class EHuntingClueType {
    Footprint = 0,
    BloodTrail = 1,
    EHuntingClueType_MAX = 2,
};

enum class EIcarusActorDestroyReason {
    Other = 0,
    Pickup = 1,
    Durability = 2,
    EIcarusActorDestroyReason_MAX = 3,
};

enum class EIcarusClaimLaunchConfirmationStep {
    ClaimingProspect = 0,
    LoadingProspect = 1,
    EIcarusClaimLaunchConfirmationStep_MAX = 2,
};

enum class EIcarusDamageType {
    Undefined = 0,
    Pure = 1,
    Melee = 2,
    Projectile = 3,
    Fire = 4,
    FallDamage = 5,
    Collision = 6,
    Poison = 7,
    Wind = 8,
    Shield = 9,
    Returned = 10,
    Frost = 11,
    Electric = 12,
    Explosive = 13,
    Shatter = 14,
    Felling = 15,
    Laser = 16,
    EIcarusDamageType_MAX = 17,
};

enum class EIcarusGameVersionFlags {
    None = 0,
    Major = 1,
    Minor = 2,
    Patch = 4,
    Changelist = 8,
    BuildType = 16,
    FeatureLevel = 32,
    Numbers = 15,
    All = 63,
    EIcarusGameVersionFlags_MAX = 64,
};

enum class EIcarusItemContext {
    None = 0,
    World = 1,
    EquipHand = 2,
    EquipBack = 3,
    Vehicle = 4,
    Deployable = 5,
    Slotable = 6,
    Buildable = 7,
    DropshipPart = 8,
    Gravestone = 9,
    Light = 10,
    EIcarusItemContext_MAX = 11,
};

enum class EIcarusJoinConfirmationStep {
    FindingSession = 0,
    JoiningProspect = 1,
    LoadingProspect = 2,
    EIcarusJoinConfirmationStep_MAX = 3,
};

enum class EIcarusOrchestrationStateFlag {
    None = 0,
    DatabaseReloadRequired = 1,
    DatabaseReloadBegin = 2,
    DatabaseReloadComplete = 3,
    ActorsReloadedToDatabaseState = 4,
    IcarusBeginPlay = 5,
    ClearedAllConcerns = 6,
    RaiseCurtain = 7,
    GameModeBeginPlay = 8,
    AllRequiredActorsSpawned = 9,
    EIcarusOrchestrationStateFlag_MAX = 10,
};

enum class EIcarusProspectDifficulty {
    Easy = 0,
    Normal = 1,
    Hard = 2,
    Extreme = 3,
    EIcarusProspectDifficulty_MAX = 4,
};

enum class EIcarusResourceType {
    None = 0,
    Energy = 1,
    Water = 2,
    Fuel = 3,
    Oxygen = 4,
    Hydrazine = 5,
    Crude_Oil = 6,
    Refined_Oil = 7,
    Chute = 8,
    MaxResourceTypes = 9,
    EIcarusResourceType_MAX = 10,
};

enum class EIcarusResumeConfirmationStep {
    ResumeRequest = 0,
    ConfirmationHost = 1,
    ConfirmationJoin = 2,
    LoadingProspectHost = 3,
    FindingSessionJoin = 4,
    LoadingProspectJoin = 5,
    Mismatch = 6,
    EIcarusResumeConfirmationStep_MAX = 7,
};

enum class EIcarusWeatherDifficulty {
    Light = 0,
    Medium = 1,
    Heavy = 2,
    Extreme = 3,
    EIcarusWeatherDifficulty_MAX = 4,
};

enum class EInputContext {
    Both = 0,
    KeyboardOnly = 1,
    ControllerOnly = 2,
    EInputContext_MAX = 3,
};

enum class EInputStatSourceType {
    Aiming = 0,
    EInputStatSourceType_MAX = 1,
};

enum class EInputTypeSetting {
    Keyboard = 0,
    Controller = 1,
    NumSettings = 2,
    Custom = 255,
    EInputTypeSetting_MAX = 256,
};

enum class EInstancedLevelPickType {
    FirstItem = 0,
    EInstancedLevelPickType_MAX = 1,
};

enum class EInteractType {
    Undefined = 0,
    WorldPress = 1,
    WorldHold = 2,
    WorldAltPress = 3,
    WorldAltHold = 4,
    EInteractType_MAX = 5,
};

enum class EInteractableHitLookupType {
    None = 0,
    FLOD_Instance = 1,
    EInteractableHitLookupType_MAX = 2,
};

enum EInventorySortType {
    ByTag = 0,
    ByWeight = 1,
    ByStackCount = 2,
    ByAlphaNumeric = 3,
    EInventorySortType_MAX = 4,
};

enum class EItemCraftingTypeFMODParam {
    Player = 0,
    World = 1,
    EItemCraftingTypeFMODParam_MAX = 2,
};

enum class EItemDestructionContext {
    Decayed = 0,
    Dismantled = 1,
    FellOutOfWorld = 2,
    EItemDestructionContext_MAX = 3,
};

enum class EKeybindVisibility {
    VisibleRemap = 0,
    VisibleNoRemap = 1,
    Invisible = 2,
    EKeybindVisibility_MAX = 3,
};

enum class ELastProspectHostType {
    LocalHost = 0,
    SteamP2P = 1,
    DedicatedServer = 2,
    ELastProspectHostType_MAX = 3,
};

enum class ELeaveProspectSessionType {
    None = 0,
    Quit = 1,
    ReturnToCharacterSelect = 2,
    LeaveByDropship = 3,
    ReturnToTitlescreen = 4,
    Disconnected = 5,
    ELeaveProspectSessionType_MAX = 6,
};

enum class ELevel {
    NoLogging = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    ELevel_MAX = 4,
};

enum class ELineDrawMethod {
    Unspecified = 0,
    NoLine = 1,
    ShortestDistance = 2,
    XThenY = 3,
    YThenX = 4,
    ELineDrawMethod_MAX = 5,
};

enum class ELobbyPrivacy {
    Unknown = 0,
    FriendsOnly = 1,
    Private = 2,
    ELobbyPrivacy_MAX = 3,
};

enum class ELookAtType {
    PitchAndYaw = 0,
    VectorLocation = 1,
    AbsoluteLocation = 2,
    ELookAtType_MAX = 3,
};

enum class EMapTileRadarFlag {
    NotScanned = 0,
    NoResource = 1,
    FoundResource = 2,
    Scanning = 3,
    FogOfWar = 4,
    EMapTileRadarFlag_MAX = 5,
};

enum class EMetaHashResult {
    None = 0,
    FilesNotFound = 1,
    BadFileSize = 2,
    ExtraFilesFound = 4,
    ModFilesFound = 8,
    All = 255,
    EMetaHashResult_MAX = 256,
};

enum class EMigrationStep {
    Start = 0,
    CreatePlayerDataFolder = 1,
    MigrateMetaInventoryFormat = 2,
    OnlineGetUserProfile = 3,
    OnlineGetCharacterData = 4,
    OnlineGetCharacterLoadouts = 5,
    OnlineGetMetaInventory = 6,
    SwitchToOffline = 7,
    CacheOfflineManagers = 8,
    OfflineGetUserProfile = 9,
    OfflineGetCharacterData = 10,
    OfflineGetCharacterLoadouts = 11,
    OfflineGetMetaInventory = 12,
    MergeProfileData = 13,
    MergeCharacterData = 14,
    MergeLoadoutData = 15,
    MergeMetaInventories = 16,
    MergeOutpostFiles = 17,
    UpdateLoadoutData = 18,
    MigrateSaveFormat = 19,
    DeletingOldFiles = 20,
    FinaliseMigration = 21,
    EMigrationStep_MAX = 22,
};

enum class EMissionState {
    InProgress = 0,
    Completed = 1,
    Abandoned = 2,
    Failed = 3,
    MAX = 4,
};

enum class EModifierMergeType {
    Stack = 0,
    LongestDuration = 1,
    Replace = 2,
    Count = 3,
    EModifierMergeType_MAX = 4,
};

enum class EModifierType {
    Buff = 0,
    Debuff = 1,
    Biome = 2,
    Aura_Positive = 3,
    Aura_Negative = 4,
    Radiation = 5,
    Item = 6,
    EModifierType_MAX = 7,
};

enum class EMountAction {
    Invalid = 0,
    Eating = 1,
    Drinking = 2,
    Sleeping = 3,
    Attacking = 4,
    Escaping = 5,
    Socialising = 6,
    EMountAction_MAX = 7,
};

enum class EMountCombatBehaviourState {
    Invalid = 0,
    DoNotEngage = 1,
    NeutralEngagement = 2,
    AggressiveEngagement = 3,
    EMountCombatBehaviourState_MAX = 4,
};

enum class EMountConsumptionBehaviourState {
    Invalid = 0,
    Any = 1,
    Assigned = 2,
    None = 3,
    EMountConsumptionBehaviourState_MAX = 4,
};

enum class EMountGrazingBehaviourState {
    Invalid = 0,
    Any = 1,
    Foliage = 2,
    Carcasses = 3,
    None = 4,
    EMountGrazingBehaviourState_MAX = 5,
};

enum class EMountMovementBehaviourState {
    Invalid = 0,
    Follow = 1,
    IdleWander = 2,
    IdleStanding = 3,
    IdleLying = 4,
    EMountMovementBehaviourState_MAX = 5,
};

enum class EMovementState {
    Undefined = 0,
    Stationary = 1,
    Sneak = 2,
    Walk = 3,
    Jog = 4,
    Run = 5,
    Sprint = 6,
    Attacking = 7,
    Following = 8,
    EMovementState_MAX = 9,
};

enum class EMusicConditionCombatState {
    None = 0,
    Idle = 1,
    InCombat = 2,
    InCombat_Boss = 4,
    InCombat_EpicBoss = 8,
    EMusicConditionCombatState_MAX = 9,
};

enum class EMusicConditionDisaster {
    None = 0,
    Normal = 1,
    Fire = 2,
    EMusicConditionDisaster_MAX = 3,
};

enum class EMusicConditionDropState {
    None = 0,
    DropShipDescending = 1,
    Prospect = 2,
    DropShipAscending = 4,
    Hab = 8,
    LoadingProspect = 16,
    EMusicConditionDropState_MAX = 17,
};

enum class EMusicConditionDropTime {
    None = 0,
    Normal = 1,
    TimeRunningOut = 2,
    EMusicConditionDropTime_MAX = 3,
};

enum class EMusicConditionGameplayEvent {
    None = 0,
    DiscoveredMetaResource = 1,
    Revived = 2,
    EMusicConditionGameplayEvent_MAX = 3,
};

enum class EMusicConditionPlayerState {
    None = 0,
    Alive = 1,
    Dead = 2,
    LowHealth = 4,
    EMusicConditionPlayerState_MAX = 5,
};

enum class EMusicConditionTimeOfDay {
    None = 0,
    Dawn = 1,
    Day = 2,
    Dusk = 4,
    Night = 8,
    EMusicConditionTimeOfDay_MAX = 9,
};

enum class EMusicConditionWeather {
    None = 0,
    Normal = 1,
    Storm_Ramp = 2,
    Storm_Damage = 4,
    Storm_Chaos = 8,
    EMusicConditionWeather_MAX = 9,
};

enum class ENVIDIAReflexLowLatencySetting {
    Off = 0,
    On = 1,
    On_Plus_Boost = 2,
    NumSettings = 3,
    Custom = 255,
    ENVIDIAReflexLowLatencySetting_MAX = 256,
};

enum class ENavigationType {
    Jump = 0,
    Teleport = 1,
    MaxNavigationTypes = 2,
    ENavigationType_MAX = 3,
};

enum class EObjectSlotType {
    ObjectSlotInput = 0,
    ObjectSlotOutput = 1,
    ObjectSlotStorage = 2,
    EObjectSlotType_MAX = 3,
};

enum class EOcclusionShelterContextFMODParam {
    None = 0,
    ListenerLowSourceLow = 1,
    ListenerLowSourceMed = 2,
    ListenerLowSourceHigh = 3,
    ListenerMedSourceLow = 4,
    ListenerMedSourceMed = 5,
    ListenerMedSourceHigh = 6,
    ListenerHighSourceLow = 7,
    ListenerHighSourceMed = 8,
    ListenerHighSourceHigh = 9,
    EOcclusionShelterContextFMODParam_MAX = 10,
};

enum class EOnProspectAvailability {
    None = 0,
    Base = 1,
    Upgrade1 = 2,
    Upgrade2 = 3,
    Upgrade3 = 4,
    EOnProspectAvailability_MAX = 5,
};

enum class EOverallSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EOverallSetting_MAX = 256,
};

enum class EPayloadDeploymentType {
    OnProjectileMovementComplete = 0,
    OnLaunch = 1,
    OnBounceImmediate = 2,
    OnBounceWaitForHalt = 3,
    OnTimerElapsed = 4,
    EPayloadDeploymentType_MAX = 5,
};

enum class EPlantGrowthStates {
    Unseeded = 0,
    Stage1 = 1,
    Stage2 = 2,
    Stage3 = 3,
    Stage4 = 4,
    Mature = 5,
    Decayed = 6,
    EPlantGrowthStates_MAX = 7,
};

enum class EPlayerArmourTypeFMODParam {
    None = 0,
    Fiber = 1,
    Fur = 2,
    Leather = 3,
    Ghillie = 4,
    Carbon = 5,
    Composite = 6,
    Polar = 7,
    Scale = 8,
    Bone = 9,
    Obsidian = 10,
    Metal = 11,
    EPlayerArmourTypeFMODParam_MAX = 12,
};

enum class EPlayerAudioFoliageType {
    Undefined = 0,
    Tree = 1,
    Bush = 2,
    EPlayerAudioFoliageType_MAX = 3,
};

enum class EPlayerFoliageFMODParam {
    None = 0,
    Bush = 1,
    BushDry = 2,
    BushLow = 3,
    BushTwiggy = 4,
    Flower = 5,
    Bramble = 6,
    EPlayerFoliageFMODParam_MAX = 7,
};

enum class EPlayerGroundStateFMODParam {
    Earth = 0,
    Air = 1,
    Water = 2,
    EPlayerGroundStateFMODParam_MAX = 3,
};

enum class EPlayerStanceFMODParam {
    Jogging = 0,
    Sprinting = 1,
    Crouching = 2,
    Walking = 3,
    EPlayerStanceFMODParam_MAX = 4,
};

enum class EPlayerTypeFMODParam {
    LocalPlayerFirstPerson = 0,
    LocalPlayerThirdPerson = 1,
    OtherPlayer = 2,
    EPlayerTypeFMODParam_MAX = 3,
};

enum class EPostProcessingSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EPostProcessingSetting_MAX = 256,
};

enum class EPrimaryItemTypes {
    Generic = 0,
    Actionable = 1,
    Armor = 2,
    Ballistic = 3,
    Buildable = 4,
    Consumable = 5,
    Combustible = 6,
    Deployable = 7,
    Energy = 8,
    Equippable = 9,
    Highlightable = 10,
    Interactable = 11,
    Itemable = 12,
    Meshable = 13,
    Processing = 14,
    Useable = 15,
    Weight = 16,
    Tool = 17,
    Resource = 18,
    Rocketable = 19,
    EPrimaryItemTypes_MAX = 20,
};

enum class EProcessorPurpose {
    Crafting = 0,
    Repairing = 1,
    EProcessorPurpose_MAX = 2,
};

enum class EProcessorStoppedReason {
    GenericFailure = 0,
    NoEnergy = 1,
    NoResources = 2,
    NoRecipe = 3,
    NoQueue = 4,
    NoSpace = 5,
    NotTurnedOn = 6,
    NoResourceRemaining = 7,
    PlayerStopped = 8,
    EProcessorStoppedReason_MAX = 9,
};

enum class EProgressState {
    Prototype = 0,
    Review = 1,
    Complete = 2,
    NumStates = 3,
    EProgressState_MAX = 4,
};

enum class EProjectileBreakModifier {
    NoChange = 0,
    Unbreakable = 1,
    MustBreak = 2,
    EProjectileBreakModifier_MAX = 3,
};

enum class EProspectRequiredTech {
    None = 0,
    Tier1 = 1,
    Tier2 = 2,
    Tier3 = 3,
    Tier4 = 4,
    Tier5 = 5,
    EProspectRequiredTech_MAX = 6,
};

enum class EQuestActorState {
    Valid = 0,
    Invalid = 1,
    EQuestActorState_MAX = 2,
};

enum class EQuestModifiersTableType {
    D_QuestWeatherModifiers = 0,
    D_QuestEnemyModifiers = 1,
    D_QuestVocalisationModifiers = 2,
    None = 255,
    EQuestModifiersTableType_MAX = 256,
};

enum class EQuestState {
    Complete = 0,
    Incomplete = 1,
    EQuestState_MAX = 2,
};

enum class EQuestVocalisationType {
    InitialAudio = 0,
    UpdateAudio = 1,
    FinishAudio = 2,
    EQuestVocalisationType_MAX = 3,
};

enum class ERCONCommandContext {
    Any = 0,
    ServerLobby = 1,
    Survival = 2,
    ERCONCommandContext_MAX = 3,
};

enum class ERCONCommandPlatformContext {
    Any = 0,
    DedicatedServer = 1,
    P2P = 2,
    ERCONCommandPlatformContext_MAX = 3,
};

enum class ERateLimitedRequests {
    None = 0,
    GetDropships = 1,
    GetMetaResources = 2,
    GetMetaInventory = 3,
    GetDropInventory = 4,
    GetWorkshopPacks = 5,
    GetCredits = 6,
    GetNotifications = 7,
    SyncTalents = 8,
    ERateLimitedRequests_MAX = 9,
};

enum class ERefundPermission {
    Inherit = 0,
    Block = 1,
    Allow = 2,
    ERefundPermission_MAX = 3,
};

enum class ERefundTalentResponse {
    Invalid = 0,
    Success = 1,
    NullModel = 2,
    NotUnlocked = 3,
    IsDependency = 4,
    InvalidatesRank = 5,
    ERefundTalentResponse_MAX = 6,
};

enum class ERelationshipType {
    Neutral = 0,
    Hostile = 1,
    Friendly = 2,
    ERelationshipType_MAX = 3,
};

enum class EReloadType {
    Magazine = 0,
    Chambered = 1,
    EReloadType_MAX = 2,
};

enum class ERemoteUserSetting {
    DisableCameraFocusOnProcessor = 0,
    ERemoteUserSetting_MAX = 1,
};

enum class ERepairItemTier {
    RepairTierUnknown = 0,
    RepairTier1 = 1,
    RepairTier2 = 2,
    RepairTier3 = 3,
    RepairTier4 = 4,
    RepairTier5 = 5,
    RepairTierWorkshop = 6,
    ERepairItemTier_MAX = 7,
};

enum class ERequestPlayerPersonaErrorCode {
    NoError = 0,
    InvalidId = 1,
    RequestTimedOut = 2,
    ERequestPlayerPersonaErrorCode_MAX = 3,
};

enum class ERequestResourceComponentDataSource {
    ViewTrace = 0,
    OpenUI = 1,
    ERequestResourceComponentDataSource_MAX = 2,
};

enum class EResourceLibraryExec {
    Valid = 0,
    Invalid = 1,
    EResourceLibraryExec_MAX = 2,
};

enum class EResourceNetworkFlowType {
    Consume = 0,
    Produce = 1,
    Store = 2,
    EResourceNetworkFlowType_MAX = 3,
};

enum class EResumeStep {
    None = 0,
    AskHost = 1,
    AskJoin = 2,
    ShouldHost = 3,
    ShouldJoin = 4,
    ShouldMismatch = 5,
    EResumeStep_MAX = 6,
};

enum class ERocketPartConnectionType {
    Undefined = 0,
    MK1_TOP = 1,
    MK1_BOTTOM = 2,
    MK2_TOP = 3,
    MK2_BOTTOM = 4,
    ERocketPartConnectionType_MAX = 5,
};

enum class ERocketPartType {
    Undefined = 0,
    ERocketPartType_MAX = 1,
};

enum class ERocketState {
    Inactive = 0,
    Descending = 1,
    Landed = 2,
    Ascending = 3,
    ERocketState_MAX = 4,
};

enum class ERollResult {
    Success = 0,
    Failure = 1,
    ERollResult_MAX = 2,
};

enum class ESecondaryItemTypes {
    Generic = 0,
    Helmet = 1,
    Chest = 2,
    Gloves = 3,
    Pants = 4,
    Boots = 5,
    Envirosuit = 6,
    FoodResource = 7,
    WaterResource = 8,
    OxygenResource = 9,
    Utility = 10,
    FuelA = 11,
    FuelB = 12,
    FuelC = 13,
    ESecondaryItemTypes_MAX = 14,
};

enum class ESessionFilterState {
    None = 0,
    ShowOnly = 1,
    HideOnly = 2,
    ESessionFilterState_MAX = 3,
};

enum class ESessionSearchType {
    PlayerHosted = 0,
    Dedicated = 1,
    ESessionSearchType_MAX = 2,
};

enum class ESessionSortDirection {
    Ascending = 0,
    Descending = 1,
    ESessionSortDirection_MAX = 2,
};

enum class ESessionSortType {
    None = 0,
    LobbyName = 1,
    ProspectName = 2,
    Duration = 3,
    Difficulty = 4,
    PlayerCount = 5,
    Ping = 6,
    Hardcore = 7,
    ESessionSortType_MAX = 8,
};

enum class ESetDataSuccess {
    Success = 0,
    Failed = 1,
    ESetDataSuccess_MAX = 2,
};

enum class ESettingType {
    Bool = 0,
    Int = 1,
    Float = 2,
    Enum = 3,
    String = 4,
    ESettingType_MAX = 5,
};

enum class ESettingsCategory {
    Display = 0,
    Audio = 1,
    Gameplay = 2,
    Controls = 3,
    ESettingsCategory_MAX = 4,
};

enum class EShadingSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EShadingSetting_MAX = 256,
};

enum class EShadowFilterMethodSetting {
    PCF = 0,
    PCSS = 1,
    NumSettings = 2,
    Custom = 255,
    EShadowFilterMethodSetting_MAX = 256,
};

enum class EShadowsSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EShadowsSetting_MAX = 256,
};

enum class ESkyboxQualitySetting {
    Low = 0,
    Normal = 1,
    NumSettings = 2,
    Custom = 255,
    ESkyboxQualitySetting_MAX = 256,
};

enum class ESleepResult {
    Valid = 0,
    InvalidTime = 1,
    ESleepResult_MAX = 2,
};

enum class ESplineLoopDirection {
    Undetermined = 0,
    Anticlockwise = 1,
    Clockwise = 2,
    ESplineLoopDirection_MAX = 3,
};

enum class EStaminaBracket {
    Empty = 0,
    Low = 1,
    Normal = 2,
    Full = 3,
    EStaminaBracket_MAX = 4,
};

enum class EStatDisplayOperation {
    None = 0,
    Multiply = 1,
    Division = 2,
    Addition = 3,
    EStatDisplayOperation_MAX = 4,
};

enum class EStatSources {
    Base = 0,
    FromServer = 1,
    Armour = 2,
    Buff = 3,
    Item = 4,
    Durable = 5,
    Buildable = 6,
    DropShip = 7,
    Attributes = 8,
    Perks = 9,
    Projectile = 10,
    GOAP = 11,
    EquippedItems = 12,
    MapManager = 13,
    ArmourSetBonus = 14,
    AIManager = 15,
    Talents = 16,
    Biome = 17,
    TimeOfDay = 18,
    Weather = 19,
    BackingStatsContainer = 20,
    Weight = 21,
    World = 22,
    Ruleset = 23,
    AISetup = 24,
    AISpawner = 25,
    EpicCreature = 26,
    DamageEnabledAnimNotify = 27,
    BTTaskPerformAction = 28,
    Input = 29,
    GOAPAction = 30,
    CriticalHit = 31,
    GenericBehaviourTree = 32,
    Alteration = 33,
    TamingComponent = 34,
    Atmosphere = 35,
    Shield = 36,
    Mount = 37,
    CreatureModifiers = 38,
    BestiaryProgress = 39,
    IcarusMountCharacter = 40,
    Movement = 41,
    OffHandActor = 42,
    InstancedLevel = 43,
    Actionable = 44,
    Genetics = 45,
    EStatSources_MAX = 46,
};

enum class EStateRecorderOwnerResolvePolicy {
    FindOnly = 0,
    RespawnOnly = 1,
    FindOrRespawn = 2,
    ManuallyResolved = 3,
    EStateRecorderOwnerResolvePolicy_MAX = 4,
};

enum class EStealthAttackType {
    NoStealth = 0,
    PartialStealth = 1,
    FullStealth = 2,
    EStealthAttackType_MAX = 3,
};

enum class ESteamSearchType {
    Internet = 0,
    Favorites = 1,
    History = 2,
    Spectate = 3,
    Lan = 4,
    Friends = 5,
    ESteamSearchType_MAX = 6,
};

enum class ESuperResolutionSetting {
    Off = 0,
    Auto = 1,
    Quality = 2,
    Balanced = 3,
    Performance = 4,
    Ultra_Performance = 5,
    NumSettings = 6,
    Custom = 255,
    ESuperResolutionSetting_MAX = 256,
};

enum class ESurfaceFMODParam {
    Default = 0,
    Dirt = 1,
    Sand = 2,
    Grass = 3,
    Wood = 4,
    Rock = 5,
    Plastic = 6,
    Metal = 7,
    Carpet = 8,
    Snow = 9,
    Water = 10,
    Gravel = 11,
    Flesh = 12,
    Concrete = 13,
    Mud = 14,
    Ice = 15,
    Tree = 16,
    VoxelRock = 17,
    VoxelMetal = 18,
    Bush = 19,
    Glass = 20,
    Thatch = 21,
    Cactus = 22,
    Bone = 23,
    CorrugatedIron = 24,
    Lava = 25,
    Slime = 26,
    ESurfaceFMODParam_MAX = 27,
};

enum class ESurveyLaserFMODParam {
    LaserOff = 0,
    LaserOn = 1,
    ESurveyLaserFMODParam_MAX = 2,
};

enum class ESurveyTransmitFMODParam {
    NotTransmitting = 0,
    Transmitting = 1,
    ESurveyTransmitFMODParam_MAX = 2,
};

enum class ESurvivalConsumableType {
    Food = 0,
    Water = 1,
    Oxygen = 2,
    ESurvivalConsumableType_MAX = 3,
};

enum class ESurvivalStatType {
    Food = 0,
    Water = 1,
    Oxygen = 2,
    ESurvivalStatType_MAX = 3,
};

enum class ETagRequirement {
    HasAllTags = 0,
    HasAnyTags = 1,
    ETagRequirement_MAX = 2,
};

enum class ETalentModelStorage {
    None = 0,
    Character = 1,
    Account = 2,
    Creature = 3,
    World = 4,
    ETalentModelStorage_MAX = 5,
};

enum class ETalentNodeType {
    Talent = 0,
    Reroute = 1,
    MutuallyExclusive = 2,
    ETalentNodeType_MAX = 3,
};

enum class ETalentState {
    Locked = 0,
    Available = 1,
    Unlocked = 2,
    Completed = 3,
    ETalentState_MAX = 4,
};

enum class ETamedCreatureType {
    Juvenile = 0,
    TamedCreature = 1,
    Both = 2,
    ETamedCreatureType_MAX = 3,
};

enum class ETamedState {
    NotTamed = 0,
    Tamed = 1,
    Domesticated = 2,
    ETamedState_MAX = 3,
};

enum class ETamingTemperatureState {
    JustRight = 0,
    TooHot = 1,
    TooCold = 2,
    ETamingTemperatureState_MAX = 3,
};

enum class ETargetRangeState {
    Waiting = 0,
    Active = 1,
    ETargetRangeState_MAX = 2,
};

enum class ETerrainAnchorState {
    Undefined = 0,
    Valid = 1,
    Invalid = 2,
    ETerrainAnchorState_MAX = 3,
};

enum class ETestRailState {
    Inactive = 0,
    Initialising = 1,
    Running = 2,
    Complete = 3,
    ETestRailState_MAX = 4,
};

enum class ETexturesSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    ETexturesSetting_MAX = 256,
};

enum class ETrackerSetType {
    Overwrite = 0,
    KeepHighest = 1,
    ETrackerSetType_MAX = 2,
};

enum class ETreeDetachContextFMODParam {
    Collision = 0,
    PlayerCollision = 1,
    PlayerActionIndirect = 2,
    PlayerActionDirect = 3,
    ETreeDetachContextFMODParam_MAX = 4,
};

enum class ETreePrimitiveDetachContext {
    None = 0,
    PlayerAction_Direct = 1,
    PlayerAction_Indirect = 2,
    Collision = 3,
    Fire = 4,
    Storm = 5,
    ETreePrimitiveDetachContext_MAX = 6,
};

enum class ETreePrimitiveItemReplaceMethod {
    None = 0,
    SpawnWorldItem = 1,
    DirectIntoInventory = 2,
    ETreePrimitiveItemReplaceMethod_MAX = 3,
};

enum class ETreePrimitiveType {
    None = 0,
    Root = 1,
    Trunk = 2,
    Branch = 3,
    Leaf = 4,
    Socketable = 5,
    ETreePrimitiveType_MAX = 6,
};

enum class EUVWrapMethod {
    UV_TripleProjection = 0,
    UV_ZProjection = 1,
    UV_Spherical = 2,
    UV_MAX = 3,
};

enum class EViewDistanceSetting {
    Low = 0,
    Medium = 1,
    High = 2,
    Epic = 3,
    Cinematic = 4,
    NumSettings = 5,
    Custom = 255,
    EViewDistanceSetting_MAX = 256,
};

enum class EViewTraceHitType {
    None = 0,
    LineTrace = 1,
    VolumeTrace = 2,
    EViewTraceHitType_MAX = 3,
};

enum class EViewTraceResultPriority {
    Blocking = 0,
    Ignore = 1,
    Low = 2,
    Normal = 3,
    High = 4,
    EViewTraceResultPriority_MAX = 5,
};

enum class EVocalisationInterruptType {
    Interrupt = 0,
    Cancel = 1,
    Queue = 2,
    EVocalisationInterruptType_MAX = 3,
};

enum class EVocalisationPlayResult {
    Cancelled = 0,
    Played = 1,
    Queued = 2,
    EVocalisationPlayResult_MAX = 3,
};

enum class EVocalisationPriority {
    Lowest = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    Highest = 4,
    EVocalisationPriority_MAX = 5,
};

enum class EVoxelMinedState {
    NotMined = 0,
    PartiallyMined = 1,
    FullyMined = 2,
    EVoxelMinedState_MAX = 3,
};

enum class EVoxelResourceCategory {
    None = 0,
    Stone = 1,
    Metal = 2,
    Oxite = 3,
    Copper = 4,
    Gold = 5,
    Bauxite = 6,
    Sulfur = 7,
    Silica = 8,
    Ice = 9,
    Platinum = 10,
    Titanium = 11,
    Coal = 12,
    Exotic_A = 13,
    Salt = 14,
    Limestone = 15,
    Lithium = 16,
    Ruby = 17,
    EVoxelResourceCategory_MAX = 18,
};

enum class EWaterStoredFMODParam {
    None = 0,
    Some = 1,
    EWaterStoredFMODParam_MAX = 2,
};

enum class EWeaponAimingFMODParam {
    NotAiming = 0,
    Aiming = 1,
    EWeaponAimingFMODParam_MAX = 2,
};

enum class EWeaponChargingFMODParam {
    NotCharging = 0,
    Charging = 1,
    EWeaponChargingFMODParam_MAX = 2,
};

enum class EWeaponReloadingFMODParam {
    NotReloading = 0,
    Reloading = 1,
    EWeaponReloadingFMODParam_MAX = 2,
};

enum class EWeaponSilencedFMODParam {
    STANDARD = 0,
    SILENCED = 1,
    EWeaponSilencedFMODParam_MAX = 2,
};

enum class EWorldPlacementType {
    GroundPlacement = 0,
    WallPlacement = 1,
    WaterPlacement = 2,
    GroundOrWallPlacement = 3,
    CeilingPlacement = 4,
    LavaPlacement = 5,
    EWorldPlacementType_MAX = 6,
};

enum class RiverAudioState {
    InfrequentlyChecking = 0,
    FrequentlyChecking = 1,
    ActivelyUpdating = 2,
    RiverAudioState_MAX = 3,
};

