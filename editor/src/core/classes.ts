// Class registry: describes every instance class, its properties (type,
// default, category), its scripting events/methods and editor metadata.
// The Properties panel, serializer, Insert menus and Lua bridge are all
// driven from this table, so adding a class here makes it available everywhere.

import { udim2, v3 } from './types';

export type PropType =
  | 'string' | 'number' | 'int' | 'bool' | 'vec3' | 'color' | 'enum' | 'source' | 'asset' | 'udim2';

export type AssetKind = 'model' | 'texture' | 'sound' | 'animation' | 'script' | 'material';

export interface PropDef {
  name: string;
  type: PropType;
  default: unknown;
  category: string;
  enumValues?: readonly string[];
  assetKind?: AssetKind;
  min?: number;
  max?: number;
  step?: number;
  readOnly?: boolean;
  /** Not shown in Properties panel (still serialized). */
  hidden?: boolean;
  doc?: string;
}

export interface ClassDef {
  name: string;
  base?: string;
  abstract?: boolean;
  /** Can be created via Insert / Instance.new. */
  creatable: boolean;
  /** Services/singletons cannot be deleted, renamed or re-parented. */
  locked?: boolean;
  icon: string;
  insertCategory?: string;
  props: PropDef[];
  events?: string[];
  methods?: string[];
  description: string;
}

export const MATERIALS = [
  'Plastic', 'SmoothPlastic', 'Wood', 'WoodPlanks', 'Brick', 'Concrete', 'Cobblestone', 'Granite', 'Marble',
  'Slate', 'Metal', 'DiamondPlate', 'CorrodedMetal', 'Foil', 'Grass', 'Sand', 'Ice', 'Glass', 'Neon', 'Fabric',
] as const;
export type MaterialName = (typeof MATERIALS)[number];

const p = (name: string, type: PropType, def: unknown, category: string, extra: Partial<PropDef> = {}): PropDef =>
  ({ name, type, default: def, category, ...extra });

const GUI_PROPS: PropDef[] = [
  p('Position', 'udim2', udim2(0, 0, 0, 0), 'Layout'),
  p('Size', 'udim2', udim2(0, 120, 0, 40), 'Layout'),
  p('AnchorPointX', 'number', 0, 'Layout', { min: 0, max: 1, step: 0.05 }),
  p('AnchorPointY', 'number', 0, 'Layout', { min: 0, max: 1, step: 0.05 }),
  p('BackgroundColor3', 'color', '#ffffff', 'Appearance'),
  p('BackgroundTransparency', 'number', 0, 'Appearance', { min: 0, max: 1, step: 0.05 }),
  p('BorderColor3', 'color', '#1b2a35', 'Appearance'),
  p('BorderSizePixel', 'int', 0, 'Appearance', { min: 0, max: 20 }),
  p('CornerRadius', 'int', 0, 'Appearance', { min: 0, max: 200, doc: 'Rounded corner radius in pixels.' }),
  p('Visible', 'bool', true, 'Appearance'),
  p('ZIndex', 'int', 1, 'Appearance'),
  p('BackgroundBlur', 'number', 0, 'Effects', { min: 0, max: 60, step: 1, doc: 'Blurs whatever is behind the element (frosted glass), in pixels.' }),
  p('GradientEnabled', 'bool', false, 'Effects'),
  p('GradientColor', 'color', '#2f7cf6', 'Effects', { doc: 'Second gradient color (the first is BackgroundColor3).' }),
  p('GradientRotation', 'number', 90, 'Effects', { min: 0, max: 360, step: 15 }),
  p('StrokeThickness', 'number', 0, 'Effects', { min: 0, max: 20, step: 0.5 }),
  p('StrokeColor', 'color', '#ffffff', 'Effects'),
  p('StrokeTransparency', 'number', 0.5, 'Effects', { min: 0, max: 1, step: 0.05 }),
  p('ShadowSize', 'number', 0, 'Effects', { min: 0, max: 80, step: 1, doc: 'Soft drop shadow in pixels.' }),
  p('ShadowTransparency', 'number', 0.5, 'Effects', { min: 0, max: 1, step: 0.05 }),
];
const TEXT_PROPS: PropDef[] = [
  p('Text', 'string', 'Label', 'Text'),
  p('TextColor3', 'color', '#1b1b1b', 'Text'),
  p('TextSize', 'int', 18, 'Text', { min: 1, max: 100 }),
  p('TextScaled', 'bool', false, 'Text'),
  p('Font', 'enum', 'Sans', 'Text', { enumValues: ['Sans', 'SansBold', 'Serif', 'Mono', 'Cartoon'] }),
  p('TextXAlignment', 'enum', 'Center', 'Text', { enumValues: ['Left', 'Center', 'Right'] }),
];

const PART_EVENTS = ['Touched', 'TouchEnded', 'Changed', 'ChildAdded', 'ChildRemoved', 'AncestryChanged', 'Destroying'];
const INSTANCE_METHODS = [
  'Destroy', 'Clone', 'FindFirstChild', 'FindFirstChildOfClass', 'FindFirstChildWhichIsA', 'FindFirstAncestor',
  'FindFirstAncestorOfClass', 'WaitForChild', 'GetChildren', 'GetDescendants', 'IsA', 'IsDescendantOf',
  'GetFullName', 'ClearAllChildren', 'GetPropertyChangedSignal', 'GetAttribute', 'SetAttribute',
];

const DEFS: ClassDef[] = [
  { name: 'Instance', abstract: true, creatable: false, icon: 'instance', description: 'Base class of all objects.',
    props: [p('Name', 'string', 'Instance', 'Data'), p('ClassName', 'string', '', 'Data', { readOnly: true })],
    events: ['Changed', 'ChildAdded', 'ChildRemoved', 'AncestryChanged', 'Destroying'], methods: INSTANCE_METHODS },
  { name: 'DataModel', base: 'Instance', creatable: false, locked: true, icon: 'game', description: 'Root of a scene ("game").', props: [],
    methods: ['GetService'] },

  // ---- Services -------------------------------------------------------
  { name: 'Workspace', base: 'Instance', creatable: false, locked: true, icon: 'workspace', description: 'Holds everything that exists in the 3D world.',
    props: [
      p('Gravity', 'number', 196.2, 'Physics', { min: 0, max: 1000, doc: 'Units per second². World maps use meters: 9.81.' }),
      p('FallenPartsDestroyHeight', 'number', -500, 'Physics'),
      p('GeoLat', 'number', 48.2082, 'World', { step: 0.0001, doc: 'Latitude of the world origin (0,0,0) on the real map.' }),
      p('GeoLon', 'number', 16.3738, 'World', { step: 0.0001, doc: 'Longitude of the world origin (0,0,0) on the real map.' }),
      p('GeoElevation', 'number', 0, 'World', { doc: 'Real elevation in meters above sea level that corresponds to Y = 0.' }),
      p('PlaceName', 'string', '', 'World'),
    ], methods: ['Raycast', 'GetPartsInRadius'] },
  { name: 'Lighting', base: 'Instance', creatable: false, locked: true, icon: 'lighting', description: 'Global lighting, sky and fog.',
    props: [
      p('ClockTime', 'number', 14, 'Appearance', { min: 0, max: 24, step: 0.25, doc: 'Time of day in hours; drives the sun direction.' }),
      p('Brightness', 'number', 2, 'Appearance', { min: 0, max: 10, step: 0.1 }),
      p('Ambient', 'color', '#5a5a64', 'Appearance'),
      p('OutdoorAmbient', 'color', '#8a8f99', 'Appearance'),
      p('GlobalShadows', 'bool', true, 'Appearance'),
      p('ExposureCompensation', 'number', 0, 'Appearance', { min: -3, max: 3, step: 0.1 }),
      p('FogColor', 'color', '#c0c8d0', 'Fog'),
      p('FogStart', 'number', 0, 'Fog', { min: 0 }),
      p('FogEnd', 'number', 100000, 'Fog', { min: 0 }),
    ] },
  { name: 'ReplicatedStorage', base: 'Instance', creatable: false, locked: true, icon: 'storage', description: 'Container for shared objects and modules.', props: [] },
  { name: 'ServerStorage', base: 'Instance', creatable: false, locked: true, icon: 'storage', description: 'Container for objects that are not rendered.', props: [] },
  { name: 'ServerScriptService', base: 'Instance', creatable: false, locked: true, icon: 'scriptservice', description: 'Scripts here run when the game starts.', props: [] },
  { name: 'StarterGui', base: 'Instance', creatable: false, locked: true, icon: 'startergui', description: 'ScreenGuis here are copied to the player at spawn.', props: [] },
  { name: 'StarterPlayer', base: 'Instance', creatable: false, locked: true, icon: 'starterplayer', description: 'Player and character defaults.',
    props: [
      p('CharacterWalkSpeed', 'number', 16, 'Character', { min: 0, max: 200 }),
      p('CharacterJumpPower', 'number', 50, 'Character', { min: 0, max: 500 }),
      p('CharacterMaxHealth', 'number', 100, 'Character', { min: 1 }),
      p('CameraMode', 'enum', 'Classic', 'Camera', { enumValues: ['Classic', 'LockFirstPerson'] }),
      p('CameraMaxZoomDistance', 'number', 128, 'Camera', { min: 0.5, max: 1000 }),
      p('CameraMinZoomDistance', 'number', 0.5, 'Camera', { min: 0.5, max: 1000 }),
      p('HeadColor', 'color', '#f5cd30', 'Avatar'),
      p('TorsoColor', 'color', '#0d69ac', 'Avatar'),
      p('LimbColor', 'color', '#f5cd30', 'Avatar'),
      p('LegColor', 'color', '#a4bd47', 'Avatar'),
    ] },
  { name: 'StarterPlayerScripts', base: 'Instance', creatable: false, locked: true, icon: 'folder', description: 'LocalScripts here run for the local player.', props: [] },
  { name: 'Players', base: 'Instance', creatable: false, locked: true, icon: 'players', description: 'Holds Player objects while the game runs.',
    props: [p('RespawnTime', 'number', 3, 'Data', { min: 0, max: 60 })], events: ['PlayerAdded', 'PlayerRemoving'], methods: ['GetPlayers', 'GetPlayerFromCharacter'] },
  { name: 'SoundService', base: 'Instance', creatable: false, locked: true, icon: 'sound', description: 'Global audio settings.',
    props: [p('Volume', 'number', 1, 'Data', { min: 0, max: 2, step: 0.05, doc: 'Master volume multiplier.' })] },
  { name: 'Terrain', base: 'Instance', creatable: false, locked: true, icon: 'terrain', description: 'Heightmap terrain with painted materials and water.',
    props: [
      p('WaterColor', 'color', '#1f6aa5', 'Water'),
      p('WaterTransparency', 'number', 0.35, 'Water', { min: 0, max: 1, step: 0.05 }),
      p('WaterLevel', 'number', 2, 'Water', { doc: 'World Y of water surfaces.' }),
      p('Decoration', 'bool', true, 'Appearance', { doc: 'Adds subtle detail noise to terrain materials.' }),
    ] },

  // ---- Environment ----------------------------------------------------
  { name: 'Sky', base: 'Instance', creatable: true, icon: 'sky', insertCategory: 'Environment', description: 'Procedural sky gradient (place in Lighting).',
    props: [
      p('TopColor', 'color', '#3d7fd6', 'Appearance'),
      p('HorizonColor', 'color', '#c4e1ff', 'Appearance'),
      p('GroundColor', 'color', '#5f6168', 'Appearance'),
      p('SunVisible', 'bool', true, 'Appearance'),
      p('SunSize', 'number', 1, 'Appearance', { min: 0.1, max: 5, step: 0.1 }),
      p('StarsVisible', 'bool', true, 'Appearance', { doc: 'Stars appear at night (ClockTime).' }),
    ] },
  { name: 'Atmosphere', base: 'Instance', creatable: true, icon: 'atmosphere', insertCategory: 'Environment', description: 'Distance haze (place in Lighting).',
    props: [
      p('Density', 'number', 0.3, 'Appearance', { min: 0, max: 1, step: 0.05 }),
      p('Color', 'color', '#c7d6e8', 'Appearance'),
    ] },

  // ---- Parts ----------------------------------------------------------
  { name: 'BasePart', base: 'Instance', abstract: true, creatable: false, icon: 'part', description: 'A physical 3D object.',
    props: [
      p('Position', 'vec3', v3(0, 0.5, 0), 'Transform'),
      p('Orientation', 'vec3', v3(0, 0, 0), 'Transform', { doc: 'Rotation in degrees (applied Y, then X, then Z).' }),
      p('Size', 'vec3', v3(4, 1, 2), 'Transform'),
      p('Color', 'color', '#a3a2a5', 'Appearance'),
      p('Material', 'enum', 'Plastic', 'Appearance', { enumValues: MATERIALS }),
      p('Transparency', 'number', 0, 'Appearance', { min: 0, max: 1, step: 0.05 }),
      p('Reflectance', 'number', 0, 'Appearance', { min: 0, max: 1, step: 0.05 }),
      p('TextureId', 'asset', '', 'Appearance', { assetKind: 'texture', doc: 'Optional texture asset tiled over the part.' }),
      p('TextureTiling', 'number', 4, 'Appearance', { min: 0.1, step: 0.5, doc: 'Studs per texture repeat.' }),
      p('Visible', 'bool', true, 'Appearance'),
      p('CastShadow', 'bool', true, 'Appearance'),
      p('Anchored', 'bool', false, 'Behavior'),
      p('CanCollide', 'bool', true, 'Behavior'),
      p('CanTouch', 'bool', true, 'Behavior'),
      p('Locked', 'bool', false, 'Behavior', { doc: 'Locked parts cannot be selected in the viewport.' }),
      p('MapId', 'string', '', 'Map', { hidden: true, doc: 'Stable id in the worldridr-map repository.' }),
      p('MapExtra', 'string', '', 'Map', { hidden: true, doc: 'Unknown map fields, kept so nothing is lost on save.' }),
      p('Density', 'number', 0.7, 'Physics', { min: 0.01, max: 100, step: 0.1 }),
      p('Friction', 'number', 0.3, 'Physics', { min: 0, max: 2, step: 0.05 }),
      p('Elasticity', 'number', 0.2, 'Physics', { min: 0, max: 1, step: 0.05 }),
      p('AssemblyLinearVelocity', 'vec3', v3(0, 0, 0), 'Physics'),
    ],
    events: PART_EVENTS, methods: [...INSTANCE_METHODS, 'GetMass', 'ApplyImpulse', 'GetTouchingParts'] },
  { name: 'Part', base: 'BasePart', creatable: true, icon: 'part', insertCategory: 'Parts', description: 'Block, ball or cylinder part.',
    props: [p('Shape', 'enum', 'Block', 'Part', { enumValues: ['Block', 'Ball', 'Cylinder'] })] },
  { name: 'WedgePart', base: 'BasePart', creatable: true, icon: 'wedge', insertCategory: 'Parts', description: 'A ramp-shaped part.', props: [] },
  { name: 'SpawnLocation', base: 'Part', creatable: true, icon: 'spawn', insertCategory: 'Parts', description: 'Players spawn on top of this part.',
    props: [p('Enabled', 'bool', true, 'Spawn')] },
  { name: 'MeshPart', base: 'BasePart', creatable: true, icon: 'mesh', insertCategory: 'Parts', description: 'A part rendered from an imported glTF/GLB model asset.',
    props: [
      p('MeshId', 'asset', '', 'Part', { assetKind: 'model' }),
      p('UseModelMaterials', 'bool', true, 'Appearance', { doc: 'Use the materials of the imported model instead of Color/Material.' }),
    ] },

  // ---- World (worldridr) ------------------------------------------------
  { name: 'Building', base: 'BasePart', creatable: true, icon: 'building', insertCategory: 'World', description: 'A building (or water/area) extruded from a footprint polygon, with a roof. Drawn in merged chunks, so thousands stay fast.',
    props: [
      p('Kind', 'enum', 'Building', 'Building', { enumValues: ['Building', 'Water', 'Area'], doc: 'Water and Area are flat, non-solid ground cover.' }),
      p('Footprint', 'string', '[-0.5,-0.5,0.5,-0.5,0.5,0.5,-0.5,0.5]', 'Building', { hidden: true, doc: 'Footprint polygon, flat list x,z,x,z… in -0.5..0.5 of the part size.' }),
      p('RoofShape', 'enum', 'Flat', 'Building', { enumValues: ['Flat', 'Gabled', 'Pyramid', 'Skillion'] }),
      p('RoofHeight', 'number', 0, 'Building', { min: 0, step: 0.25, doc: 'Meters of the part height that belong to the roof.' }),
      p('RoofColor', 'color', '#8a4b3a', 'Building'),
      p('Levels', 'int', 0, 'Building', { min: 0, max: 200 }),
      p('OsmId', 'string', '', 'Building', { readOnly: true }),
      p('MapExtra', 'string', '', 'Building', { hidden: true }),
    ] },
  { name: 'Road', base: 'BasePart', creatable: true, icon: 'road', insertCategory: 'World', description: 'A road polyline draped on the terrain. Use the Road tool to draw, drag the white handles to edit.',
    props: [
      p('RoadType', 'enum', 'Stadtstraße', 'Road', { enumValues: ['Stadtstraße', 'Landstraße', '4 Spuren', 'Einbahn', 'Autobahn', 'Feldweg'], doc: 'The road type of the worldridr game (width, lanes, speed, sidewalks).' }),
      p('Points', 'string', '[0,0,0,10,0,0]', 'Road', { hidden: true, doc: 'Flat list x,z,lift,x,z,lift… in part-local meters (lift = height above the terrain).' }),
      p('StreetName', 'string', '', 'Road'),
      p('OsmId', 'string', '', 'Road', { readOnly: true }),
      p('MapExtra', 'string', '', 'Road', { hidden: true }),
    ] },
  { name: 'GameMesh', base: 'BasePart', creatable: false, icon: 'mesh', description: 'A free-form mesh object loaded from a worldridr map (faces with colors).',
    props: [
      p('MeshData', 'string', '{"v":[],"f":[]}', 'Mesh', { hidden: true }),
      p('MapExtra', 'string', '', 'Mesh', { hidden: true }),
    ] },

  // ---- Containers -----------------------------------------------------
  { name: 'Model', base: 'Instance', creatable: true, icon: 'model', insertCategory: 'Containers', description: 'Groups parts; selecting a part in the viewport selects its model.', props: [],
    methods: [...INSTANCE_METHODS, 'GetPivot', 'PivotTo', 'MoveTo', 'GetBoundingBox'] },
  { name: 'Folder', base: 'Instance', creatable: true, icon: 'folder', insertCategory: 'Containers', description: 'Organizes objects.', props: [] },

  // ---- Scripts --------------------------------------------------------
  { name: 'LuaSourceContainer', base: 'Instance', abstract: true, creatable: false, icon: 'script', description: 'Base for scripts.',
    props: [p('Source', 'source', '', 'Script', { hidden: true })] },
  { name: 'Script', base: 'LuaSourceContainer', creatable: true, icon: 'script', insertCategory: 'Scripts', description: 'Runs when the game starts (Workspace, ServerScriptService).',
    props: [p('Enabled', 'bool', true, 'Behavior')] },
  { name: 'LocalScript', base: 'LuaSourceContainer', creatable: true, icon: 'localscript', insertCategory: 'Scripts', description: 'Runs for the local player (StarterPlayerScripts, StarterGui, character).',
    props: [p('Enabled', 'bool', true, 'Behavior')] },
  { name: 'ModuleScript', base: 'LuaSourceContainer', creatable: true, icon: 'modulescript', insertCategory: 'Scripts', description: 'Returns a value to require().', props: [] },

  // ---- Lights ---------------------------------------------------------
  { name: 'Light', base: 'Instance', abstract: true, creatable: false, icon: 'light', description: 'Base for lights; parent to a part.',
    props: [
      p('Enabled', 'bool', true, 'Light'),
      p('Brightness', 'number', 1, 'Light', { min: 0, max: 40, step: 0.1 }),
      p('Color', 'color', '#ffffff', 'Light'),
      p('Range', 'number', 16, 'Light', { min: 0, max: 120 }),
      p('Shadows', 'bool', false, 'Light'),
    ] },
  { name: 'PointLight', base: 'Light', creatable: true, icon: 'pointlight', insertCategory: 'Lights', description: 'Omnidirectional light.', props: [] },
  { name: 'SpotLight', base: 'Light', creatable: true, icon: 'spotlight', insertCategory: 'Lights', description: 'Cone light along a face of its part.',
    props: [
      p('Angle', 'number', 90, 'Light', { min: 1, max: 180 }),
      p('Face', 'enum', 'Front', 'Light', { enumValues: ['Front', 'Back', 'Top', 'Bottom', 'Left', 'Right'] }),
    ] },

  // ---- Effects --------------------------------------------------------
  { name: 'ParticleEmitter', base: 'Instance', creatable: true, icon: 'particles', insertCategory: 'Effects', description: 'Emits particles from its parent part.',
    props: [
      p('Enabled', 'bool', true, 'Emission'),
      p('Rate', 'number', 20, 'Emission', { min: 0, max: 500 }),
      p('Lifetime', 'number', 2, 'Emission', { min: 0.05, max: 20, step: 0.1 }),
      p('Speed', 'number', 5, 'Emission', { min: 0, max: 200 }),
      p('SpreadAngle', 'number', 25, 'Emission', { min: 0, max: 180 }),
      p('Acceleration', 'vec3', v3(0, 0, 0), 'Motion'),
      p('Drag', 'number', 0, 'Motion', { min: 0, max: 10, step: 0.1 }),
      p('Color', 'color', '#ffd27a', 'Appearance'),
      p('ColorEnd', 'color', '#ff5a1f', 'Appearance'),
      p('Size', 'number', 1, 'Appearance', { min: 0, max: 50, step: 0.1 }),
      p('SizeEnd', 'number', 0.2, 'Appearance', { min: 0, max: 50, step: 0.1 }),
      p('Transparency', 'number', 0, 'Appearance', { min: 0, max: 1, step: 0.05 }),
      p('TransparencyEnd', 'number', 1, 'Appearance', { min: 0, max: 1, step: 0.05 }),
      p('Shape', 'enum', 'Circle', 'Appearance', { enumValues: ['Circle', 'Spark', 'Smoke', 'Star', 'Square'] }),
      p('LightEmission', 'number', 1, 'Appearance', { min: 0, max: 1, step: 0.1, doc: '> 0.5 uses additive blending.' }),
    ], methods: [...INSTANCE_METHODS, 'Emit', 'Clear'] },
  { name: 'Sound', base: 'Instance', creatable: true, icon: 'sound', insertCategory: 'Effects', description: 'Plays an audio asset; positional when inside a part.',
    props: [
      p('SoundId', 'asset', '', 'Sound', { assetKind: 'sound' }),
      p('Volume', 'number', 0.5, 'Sound', { min: 0, max: 10, step: 0.05 }),
      p('PlaybackSpeed', 'number', 1, 'Sound', { min: 0.1, max: 4, step: 0.05 }),
      p('Looped', 'bool', false, 'Sound'),
      p('Playing', 'bool', false, 'Sound', { doc: 'If true when the game starts, the sound starts playing.' }),
      p('RollOffMaxDistance', 'number', 100, 'Sound', { min: 1, max: 10000 }),
    ], events: ['Ended', 'Played', 'Changed'], methods: [...INSTANCE_METHODS, 'Play', 'Stop', 'Pause', 'Resume'] },
  { name: 'KeyframeAnimation', base: 'Instance', creatable: true, icon: 'animation', insertCategory: 'Effects', description: 'Plays an animation asset (keyframed offsets) on its parent part or model.',
    props: [
      p('AnimationId', 'asset', '', 'Animation', { assetKind: 'animation' }),
      p('AutoPlay', 'bool', true, 'Animation'),
      p('Looped', 'bool', true, 'Animation'),
      p('Speed', 'number', 1, 'Animation', { min: 0, max: 10, step: 0.1 }),
    ], events: ['Stopped', 'Changed'], methods: [...INSTANCE_METHODS, 'Play', 'Stop'] },
  { name: 'Decal', base: 'Instance', creatable: true, icon: 'imagelabel', insertCategory: 'Effects', description: 'Shows an image on one face of its parent part.',
    props: [
      p('Texture', 'asset', '', 'Appearance', { assetKind: 'texture' }),
      p('Face', 'enum', 'Front', 'Appearance', { enumValues: ['Front', 'Back', 'Top', 'Bottom', 'Left', 'Right'] }),
      p('Color3', 'color', '#ffffff', 'Appearance', { doc: 'Tint multiplied with the image.' }),
      p('Transparency', 'number', 0, 'Appearance', { min: 0, max: 1, step: 0.05 }),
    ] },
  { name: 'RemoteEvent', base: 'Instance', creatable: true, icon: 'remote', insertCategory: 'Interaction', description: 'Sends messages between server Scripts and LocalScripts (multiplayer).',
    props: [], events: ['OnServerEvent', 'OnClientEvent', 'Changed'], methods: ['FireServer', 'FireClient', 'FireAllClients', 'Destroy', 'GetFullName', 'IsA', 'FindFirstChild', 'Clone'] },
  { name: 'ClickDetector', base: 'Instance', creatable: true, icon: 'click', insertCategory: 'Interaction', description: 'Fires MouseClick when the player clicks its parent part.',
    props: [p('MaxActivationDistance', 'number', 32, 'Data', { min: 0, max: 1000 })], events: ['MouseClick', 'Changed'] },
  { name: 'Camera', base: 'Instance', creatable: true, icon: 'camera', insertCategory: 'Environment', description: 'The first Camera in Workspace is the game camera (workspace.CurrentCamera).',
    props: [
      p('Position', 'vec3', v3(0, 12, 24), 'Transform'),
      p('Orientation', 'vec3', v3(-20, 0, 0), 'Transform'),
      p('FieldOfView', 'number', 70, 'Camera', { min: 1, max: 120 }),
      p('CameraType', 'enum', 'Custom', 'Camera', { enumValues: ['Custom', 'Fixed', 'Scriptable'], doc: 'Custom follows the player; Fixed/Scriptable render from this camera.' }),
    ], methods: [...INSTANCE_METHODS, 'LookAt'] },

  // ---- GUI ------------------------------------------------------------
  { name: 'ScreenGui', base: 'Instance', creatable: true, icon: 'screengui', insertCategory: 'GUI', description: 'Container for 2D interface drawn over the screen.',
    props: [p('Enabled', 'bool', true, 'Data'), p('DisplayOrder', 'int', 0, 'Data')] },
  { name: 'GuiObject', base: 'Instance', abstract: true, creatable: false, icon: 'frame', description: 'Base for 2D UI elements.', props: GUI_PROPS,
    events: ['MouseEnter', 'MouseLeave', 'Changed'] },
  { name: 'Frame', base: 'GuiObject', creatable: true, icon: 'frame', insertCategory: 'GUI', description: 'A rectangle.', props: [] },
  { name: 'TextLabel', base: 'GuiObject', creatable: true, icon: 'textlabel', insertCategory: 'GUI', description: 'Displays text.', props: TEXT_PROPS },
  { name: 'TextButton', base: 'GuiObject', creatable: true, icon: 'textbutton', insertCategory: 'GUI', description: 'Clickable text.', props: TEXT_PROPS,
    events: ['MouseButton1Click', 'MouseEnter', 'MouseLeave', 'Changed'] },
  { name: 'ImageLabel', base: 'GuiObject', creatable: true, icon: 'imagelabel', insertCategory: 'GUI', description: 'Displays a texture asset.',
    props: [p('Image', 'asset', '', 'Image', { assetKind: 'texture' }), p('ImageTransparency', 'number', 0, 'Image', { min: 0, max: 1, step: 0.05 })] },

  // ---- Values ---------------------------------------------------------
  { name: 'BoolValue', base: 'Instance', creatable: true, icon: 'value', insertCategory: 'Values', description: 'Stores a boolean.', props: [p('Value', 'bool', false, 'Data')] },
  { name: 'IntValue', base: 'Instance', creatable: true, icon: 'value', insertCategory: 'Values', description: 'Stores an integer.', props: [p('Value', 'int', 0, 'Data')] },
  { name: 'NumberValue', base: 'Instance', creatable: true, icon: 'value', insertCategory: 'Values', description: 'Stores a number.', props: [p('Value', 'number', 0, 'Data')] },
  { name: 'StringValue', base: 'Instance', creatable: true, icon: 'value', insertCategory: 'Values', description: 'Stores a string.', props: [p('Value', 'string', '', 'Data')] },
  { name: 'Vector3Value', base: 'Instance', creatable: true, icon: 'value', insertCategory: 'Values', description: 'Stores a Vector3.', props: [p('Value', 'vec3', v3(), 'Data')] },
  { name: 'Color3Value', base: 'Instance', creatable: true, icon: 'value', insertCategory: 'Values', description: 'Stores a Color3.', props: [p('Value', 'color', '#ffffff', 'Data')] },

  // ---- Runtime-only ---------------------------------------------------
  { name: 'RunService', base: 'Instance', creatable: false, locked: true, icon: 'scriptservice', description: 'Frame events (play mode).', props: [],
    events: ['Heartbeat', 'Stepped', 'RenderStepped'], methods: ['IsRunning', 'IsStudio', 'IsClient', 'IsServer'] },
  { name: 'UserInputService', base: 'Instance', creatable: false, locked: true, icon: 'scriptservice', description: 'Keyboard and mouse input (play mode).', props: [],
    events: ['InputBegan', 'InputEnded', 'InputChanged'], methods: ['IsKeyDown', 'IsMouseButtonPressed', 'GetMouseLocation'] },
  { name: 'TweenService', base: 'Instance', creatable: false, locked: true, icon: 'animation', description: 'Interpolates properties over time.', props: [], methods: ['Create'] },
  { name: 'DataStoreService', base: 'Instance', creatable: false, locked: true, icon: 'storage', description: 'Persistent data per game (stored on the Forge Server).', props: [], methods: ['GetDataStore'] },
  { name: 'DataStore', base: 'Instance', creatable: false, icon: 'storage', description: 'A named key/value store.', props: [], methods: ['GetAsync', 'SetAsync', 'IncrementAsync', 'RemoveAsync', 'UpdateAsync'] },
  { name: 'Debris', base: 'Instance', creatable: false, locked: true, icon: 'folder', description: 'Destroys objects after a delay.', props: [], methods: ['AddItem'] },
  { name: 'Player', base: 'Instance', creatable: false, icon: 'player', description: 'The local player during a test session.',
    props: [p('UserId', 'int', 1, 'Data', { readOnly: true }), p('DisplayName', 'string', 'Player1', 'Data')],
    events: ['CharacterAdded', 'CharacterRemoving', 'Changed'], methods: [...INSTANCE_METHODS, 'LoadCharacter', 'GetMouse'] },
  { name: 'PlayerGui', base: 'Instance', creatable: false, icon: 'startergui', description: 'Holds the player\'s ScreenGuis.', props: [] },
  { name: 'Humanoid', base: 'Instance', creatable: false, icon: 'humanoid', description: 'Character controller state.',
    props: [
      p('Health', 'number', 100, 'Data', { min: 0 }),
      p('MaxHealth', 'number', 100, 'Data', { min: 1 }),
      p('WalkSpeed', 'number', 16, 'Data', { min: 0 }),
      p('JumpPower', 'number', 50, 'Data', { min: 0 }),
      p('MoveDirection', 'vec3', v3(), 'Data', { readOnly: true }),
      p('FloorMaterial', 'string', 'Air', 'Data', { readOnly: true }),
    ], events: ['Died', 'HealthChanged', 'Jumping', 'Changed'], methods: [...INSTANCE_METHODS, 'TakeDamage', 'MoveTo', 'Jump'] },
];

const registry = new Map<string, ClassDef>(DEFS.map((d) => [d.name, d]));
const propCache = new Map<string, PropDef[]>();

export function getClass(name: string): ClassDef | undefined { return registry.get(name); }
export function allClasses(): ClassDef[] { return DEFS; }
export function creatableClasses(): ClassDef[] { return DEFS.filter((d) => d.creatable && !d.abstract); }

/** Chain from the class itself up to Instance. */
export function classChain(name: string): string[] {
  const out: string[] = [];
  let c = registry.get(name);
  while (c) { out.push(c.name); c = c.base ? registry.get(c.base) : undefined; }
  return out;
}

export function isA(className: string, base: string): boolean { return classChain(className).includes(base); }

/** All properties including inherited ones, base-class properties first. */
export function getProps(className: string): PropDef[] {
  const cached = propCache.get(className);
  if (cached) return cached;
  const chain = classChain(className).reverse();
  const map = new Map<string, PropDef>();
  for (const cn of chain) for (const pd of registry.get(cn)!.props) map.set(pd.name, pd);
  const list = [...map.values()];
  propCache.set(className, list);
  return list;
}

export function getPropDef(className: string, prop: string): PropDef | undefined {
  return getProps(className).find((d) => d.name === prop);
}

export function classEvents(className: string): string[] {
  const s = new Set<string>();
  for (const cn of classChain(className)) for (const e of registry.get(cn)?.events ?? []) s.add(e);
  return [...s];
}
export function classMethods(className: string): string[] {
  const s = new Set<string>();
  for (const cn of classChain(className)) for (const m of registry.get(cn)?.methods ?? []) s.add(m);
  return [...s];
}

/** Defaults that differ per concrete class (e.g. SpawnLocation size). */
export const CLASS_DEFAULT_OVERRIDES: Record<string, Record<string, unknown>> = {
  Part: { Size: v3(4, 1, 2) },
  WedgePart: { Size: v3(4, 1, 2) },
  SpawnLocation: { Size: v3(12, 1, 12), Anchored: true, Color: '#a3a2a5', Material: 'SmoothPlastic' },
  MeshPart: { Size: v3(4, 4, 4) },
  Building: { Size: v3(10, 7, 10), Anchored: true, Color: '#d9d2c5', Material: 'Concrete' },
  Road: { Size: v3(10, 0.2, 2), Anchored: true, Color: '#3b3d42', CanCollide: false, CastShadow: false, Material: 'Concrete' },
  GameMesh: { Size: v3(4, 4, 4), Anchored: true },
  Script: { Source: 'print("Hello world!")\n' },
  LocalScript: { Source: 'print("Hello from a LocalScript!")\n' },
  ModuleScript: { Source: 'local module = {}\n\nreturn module\n' },
  TextButton: { Text: 'Button', BackgroundColor3: '#3d8bfd', TextColor3: '#ffffff' },
  Frame: { Size: udim2(0, 200, 0, 120), BackgroundColor3: '#2b2d31', BackgroundTransparency: 0.1 },
  ImageLabel: { Size: udim2(0, 100, 0, 100), BackgroundTransparency: 1 },
};
