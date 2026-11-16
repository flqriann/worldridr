// Toolbox catalog: everything a user can drop into a scene, grouped in clear
// categories with a description, search keywords (English + German) and the
// container the item ends up in.

import { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import { SCRIPT_TEMPLATES, builtinId } from '../assets/builtin';
import type { Studio } from './Studio';
import { FUEL_SCRIPT, MESSAGE_SCRIPT, SHOP_SCRIPT, TELEPORT_SCRIPT } from '../world/zoneScripts';
import { msg, t } from '../i18n';

export type ToolboxCategory = 'parts' | 'media' | 'gameplay' | 'models' | 'lighting' | 'effects' | 'scripts' | 'ui' | 'environment';

export const TOOLBOX_CATEGORIES: { id: ToolboxCategory; label: string; icon: string; hint: string }[] = [
  { id: 'parts', label: msg('Parts'), icon: 'part', hint: msg('Basic building blocks') },
  { id: 'media', label: msg('Media'), icon: 'imagelabel', hint: msg('Your own images, sounds and 3D models') },
  { id: 'gameplay', label: msg('Gameplay'), icon: 'spawn', hint: msg('Ready-to-use objects with scripts') },
  { id: 'models', label: msg('Models'), icon: 'model', hint: msg('Decoration and props') },
  { id: 'lighting', label: msg('Lighting'), icon: 'pointlight', hint: msg('Lamps and lights') },
  { id: 'effects', label: msg('Effects'), icon: 'particles', hint: msg('Particles, sounds, animations') },
  { id: 'scripts', label: msg('Scripts'), icon: 'script', hint: msg('Code templates') },
  { id: 'ui', label: msg('UI'), icon: 'screengui', hint: msg('2D interface on screen') },
  { id: 'environment', label: msg('Environment'), icon: 'sky', hint: msg('Sky, fog, camera, values') },
];

export interface ToolboxItem {
  id: string;
  name: string;
  category: ToolboxCategory;
  description: string;
  /** Where the item goes – shown on the card. */
  target: string;
  icon: string;
  /** Built-in model asset id for a rendered thumbnail. */
  thumbAsset?: string;
  keywords: string;
  /** Needs a selected part (shown as a hint when nothing suitable is selected). */
  needsPart?: boolean;
  insert(studio: Studio, at?: Vec3): unknown;
}

const SELECTED_PART = msg('Selected part (or a new part)');

function particle(props: Record<string, unknown>) {
  return (studio: Studio, at?: Vec3) => studio.insertIntoPart('ParticleEmitter', props, at);
}

const ITEMS: ToolboxItem[] = [
  // ---- parts
  { id: 'block', name: msg('Block'), category: 'parts', icon: 'part', target: msg('Workspace'), description: msg('A rectangular part – the basic building block.'), keywords: 'block part cube box würfel quader teil baustein', insert: (s, at) => s.insertShape('Block', at) },
  { id: 'sphere', name: msg('Sphere'), category: 'parts', icon: 'ball', target: msg('Workspace'), description: msg('A round ball part.'), keywords: 'sphere ball kugel rund', insert: (s, at) => s.insertShape('Ball', at) },
  { id: 'cylinder', name: msg('Cylinder'), category: 'parts', icon: 'cylinder', target: msg('Workspace'), description: msg('A cylinder lying along the X axis.'), keywords: 'cylinder zylinder rohr säule', insert: (s, at) => s.insertShape('Cylinder', at) },
  { id: 'wedge', name: msg('Wedge'), category: 'parts', icon: 'wedge', target: msg('Workspace'), description: msg('A ramp – walkable slope.'), keywords: 'wedge ramp keil rampe schräge', insert: (s, at) => s.insertShape('Wedge', at) },
  { id: 'spawn', name: msg('Spawn Location'), category: 'parts', icon: 'spawn', target: msg('Workspace'), description: msg('Players appear on top of this pad when the game starts.'), keywords: 'spawn start player spieler startpunkt', insert: (s, at) => s.insertObject('SpawnLocation', undefined, at) },
  { id: 'model', name: msg('Empty Model'), category: 'parts', icon: 'model', target: msg('Workspace'), description: msg('Groups parts so they are selected and moved together.'), keywords: 'model group gruppe modell', insert: (s) => s.insertObject('Model') },
  { id: 'folder', name: msg('Folder'), category: 'parts', icon: 'folder', target: msg('Selected object or Workspace'), description: msg('Organizes objects in the Explorer.'), keywords: 'folder ordner organize', insert: (s) => s.insertObject('Folder') },

  // ---- media (your own files)
  { id: 'media-image', name: msg('Image from file'), category: 'media', icon: 'imagelabel', target: msg('Selected part (or a new picture panel)'), description: msg('Pick a PNG/JPG/WebP – it appears as a Decal on the selected part or on a new panel.'), keywords: 'image picture photo bild foto png jpg decal', insert: (s, at) => s.importMedia('image', undefined, at) },
  { id: 'media-sound', name: msg('Sound from file'), category: 'media', icon: 'sound', target: msg('Selected part (3D) or SoundService'), description: msg('Pick an MP3/WAV/OGG file.'), keywords: 'sound audio music musik ton mp3 wav', insert: (s) => s.importMedia('sound') },
  { id: 'media-model', name: msg('3D model from file'), category: 'media', icon: 'mesh', target: msg('Workspace'), description: msg('Pick a glTF binary (.glb) model.'), keywords: 'model 3d mesh glb gltf modell', insert: (s, at) => s.importMedia('glb', undefined, at) },
  { id: 'media-decal', name: msg('Decal'), category: 'media', icon: 'imagelabel', target: msg('Selected part'), needsPart: true, description: msg('Image on one face of a part (choose Texture in Properties).'), keywords: 'decal sticker aufkleber bild', insert: (s) => s.insertObject('Decal', s.selectedPart()) },
  { id: 'media-screenimage', name: msg('Image on screen'), category: 'media', icon: 'imagelabel', target: msg('Selected ScreenGui (or a new one)'), description: msg('2D image in the player\'s interface.'), keywords: 'image screen gui hud bild', insert: (s) => s.insertObject('ImageLabel') },

  // ---- gameplay (prefabs with scripts)
  ...([
    ['Kill Brick', msg('Kills players that touch it (red neon floor).'), 'kill brick lava death tod falle'],
    ['Jump Pad', msg('Launches players high into the air.'), 'jump pad trampolin sprung'],
    ['Coin', msg('Spinning coin – collecting it adds to the "Coins" leaderboard.'), 'coin münze collect sammeln punkte'],
    ['Checkpoint Flag', msg('Touching it makes it the respawn point.'), 'checkpoint flag flagge speicherpunkt respawn'],
    ['Moving Platform', msg('Platform that moves back and forth (tween).'), 'moving platform plattform bewegen aufzug'],
    ['Spinner', msg('Rotating deadly bar.'), 'spinner rotate drehen hindernis'],
    ['Door', msg('Door that opens when clicked.'), 'door tür click klick öffnen'],
  ]).map(([n, d, k]): ToolboxItem => ({
    id: 'prefab:' + n, name: n, category: 'gameplay', icon: 'model', thumbAsset: builtinId.model(n), target: msg('Workspace'),
    description: d, keywords: k, insert: (s, at) => s.insertPrefab(n, at),
  })),
  { id: 'zone-shop', name: 'Shop Zone', category: 'gameplay', icon: 'building', target: 'Workspace', description: 'Trigger zone with a shop script: walk in to open a buy menu (edit the items in the script).', keywords: 'shop laden geschäft kaufen supermarkt zone', insert: (s, at) => s.insertZone('Shop', '#ffb347', v3(8, 4, 8), 'ShopScript', SHOP_SCRIPT, at) },
  { id: 'zone-fuel', name: 'Fuel Station Zone', category: 'gameplay', icon: 'building', target: 'Workspace', description: 'Zone that refuels the player for money while standing inside.', keywords: 'fuel tankstelle tanken sprit zone', insert: (s, at) => s.insertZone('FuelStation', '#ff6b6b', v3(6, 3, 10), 'FuelScript', FUEL_SCRIPT, at) },
  { id: 'zone-teleport', name: 'Teleporter Zone', category: 'gameplay', icon: 'spawn', target: 'Workspace', description: 'Moves the player to another object (attribute "Target" = its name).', keywords: 'teleport teleporter portal zone', insert: (s, at) => s.insertZone('Teleporter', '#8f7bff', v3(4, 4, 4), 'TeleportScript', TELEPORT_SCRIPT, at) },
  { id: 'zone-message', name: 'Message Zone', category: 'gameplay', icon: 'textlabel', target: 'Workspace', description: 'Shows a text on screen while the player stands inside.', keywords: 'message text hinweis schild zone', insert: (s, at) => s.insertZone('MessageZone', '#4dabf7', v3(8, 4, 8), 'MessageScript', MESSAGE_SCRIPT, at) },
  { id: 'remoteevent', name: msg('Remote Event'), category: 'gameplay', icon: 'remote', target: msg('ReplicatedStorage'), description: msg('Sends messages between server Scripts and LocalScripts in multiplayer games.'), keywords: 'remote event network netzwerk multiplayer nachricht', insert: (s) => s.insertObject('RemoteEvent', s.active?.dm.getService('ReplicatedStorage')) },
  { id: 'leaderboard', name: msg('Leaderboard'), category: 'gameplay', icon: 'players', target: msg('ServerScriptService'), description: msg('Script that gives every player a "Coins" stat shown top-right.'), keywords: 'leaderboard leaderstats punkte score rangliste', insert: (s) => s.insertScriptTemplate('Leaderstats', 'service') },

  // ---- models
  ...(['Tree', 'Pine Tree', 'House', 'Lamp Post', 'Staircase', 'Bridge', 'Campfire', 'Crate Stack'] as const).map((n): ToolboxItem => ({
    id: 'prefab:' + n, name: n, category: 'models', icon: 'model', thumbAsset: builtinId.model(n), target: msg('Workspace'),
    description: { Tree: msg('Leafy tree.'), 'Pine Tree': msg('Conifer tree.'), House: msg('Small house with a clickable door and a lamp.'), 'Lamp Post': msg('Street lamp with a real light.'), Staircase: msg('10 concrete steps.'), Bridge: msg('Wooden bridge with railings.'), Campfire: msg('Logs with fire, smoke, light and crackling sound.'), 'Crate Stack': msg('Physics crates that can be pushed over.') }[n],
    keywords: { Tree: 'tree baum natur', 'Pine Tree': 'pine tanne baum', House: 'house haus gebäude', 'Lamp Post': 'lamp laterne licht straße', Staircase: 'stairs treppe stufen', Bridge: 'bridge brücke', Campfire: 'campfire lagerfeuer feuer', 'Crate Stack': 'crate kisten physik box' }[n],
    insert: (s, at) => s.insertPrefab(n, at),
  })),

  // ---- lighting
  { id: 'lamp', name: msg('Lamp'), category: 'lighting', icon: 'pointlight', target: msg('Workspace'), description: msg('Glowing block with a point light.'), keywords: 'lamp lampe licht glühbirne light', insert: (s, at) => s.insertIntoPart('PointLight', {}, at, true) },
  { id: 'pointlight', name: msg('Point Light'), category: 'lighting', icon: 'pointlight', target: SELECTED_PART, description: msg('Light shining in all directions from a part.'), keywords: 'point light punktlicht licht', insert: (s, at) => s.insertIntoPart('PointLight', {}, at) },
  { id: 'spotlight', name: msg('Spot Light'), category: 'lighting', icon: 'spotlight', target: SELECTED_PART, description: msg('Cone of light from one face of a part.'), keywords: 'spot light scheinwerfer spot strahler', insert: (s, at) => s.insertIntoPart('SpotLight', {}, at) },
  { id: 'neonlight', name: msg('Neon Strip'), category: 'lighting', icon: 'part', target: msg('Workspace'), description: msg('Thin glowing neon bar with a light.'), keywords: 'neon strip leuchte glow', insert: (s, at) => s.insertIntoPart('PointLight', { Color: '#7fdcff', Range: 18 }, at, true, { Size: v3(8, 0.4, 0.4), Color: '#7fdcff' }) },

  // ---- effects
  { id: 'fire', name: msg('Fire'), category: 'effects', icon: 'particles', target: SELECTED_PART, description: msg('Flames rising from a part.'), keywords: 'fire feuer flamme particles partikel', insert: particle({ Name: 'Fire', Rate: 40, Lifetime: 1.2, Speed: 4, SpreadAngle: 20, Acceleration: v3(0, 3, 0), Size: 1.6, SizeEnd: 0.1, Color: '#ffd27a', ColorEnd: '#ff3d00' }) },
  { id: 'smoke', name: msg('Smoke'), category: 'effects', icon: 'particles', target: SELECTED_PART, description: msg('Soft grey smoke.'), keywords: 'smoke rauch nebel', insert: particle({ Name: 'Smoke', Rate: 10, Lifetime: 4, Speed: 3, SpreadAngle: 15, Size: 1.5, SizeEnd: 6, Color: '#666666', ColorEnd: '#aaaaaa', Transparency: 0.4, TransparencyEnd: 1, Shape: 'Smoke', LightEmission: 0 }) },
  { id: 'sparkles', name: msg('Sparkles'), category: 'effects', icon: 'particles', target: SELECTED_PART, description: msg('Glittering stars.'), keywords: 'sparkles funken glitzer sterne magic', insert: particle({ Name: 'Sparkles', Rate: 25, Lifetime: 1.5, Speed: 2, SpreadAngle: 180, Size: 0.6, SizeEnd: 0, Color: '#fff6a8', ColorEnd: '#7fdcff', Shape: 'Star' }) },
  { id: 'emitter', name: msg('Particle Emitter'), category: 'effects', icon: 'particles', target: SELECTED_PART, description: msg('Customizable particle effect.'), keywords: 'particle emitter partikel effekt', insert: particle({}) },
  { id: 'sound-music', name: msg('Background Music'), category: 'effects', icon: 'sound', target: msg('SoundService'), description: msg('Looping melody that starts with the game.'), keywords: 'music musik sound ton hintergrund', insert: (s) => s.insertSound('Music', builtinId.sound('Melody'), true) },
  { id: 'sound-wind', name: msg('Ambient Wind'), category: 'effects', icon: 'sound', target: msg('SoundService'), description: msg('Looping wind ambience.'), keywords: 'wind ambient atmosphäre sound', insert: (s) => s.insertSound('Wind', builtinId.sound('Wind'), true) },
  { id: 'sound', name: msg('Sound'), category: 'effects', icon: 'sound', target: msg('Selected object or Workspace'), description: msg('Plays an audio asset (choose SoundId in Properties).'), keywords: 'sound ton audio geräusch', insert: (s) => s.insertObject('Sound') },
  { id: 'anim-spin', name: msg('Spin Animation'), category: 'effects', icon: 'animation', target: msg('Selected part/model'), needsPart: true, description: msg('Rotates the selection continuously.'), keywords: 'spin drehen rotation animation', insert: (s) => s.addAnimation(builtinId.animation('Spin'), 'Spin') },
  { id: 'anim-bob', name: msg('Hover Animation'), category: 'effects', icon: 'animation', target: msg('Selected part/model'), needsPart: true, description: msg('Floats the selection up and down.'), keywords: 'hover bob schweben animation', insert: (s) => s.addAnimation(builtinId.animation('Bob'), 'Bob') },
  { id: 'click', name: msg('Click Detector'), category: 'effects', icon: 'click', target: msg('Selected part'), needsPart: true, description: msg('Lets players click the part (MouseClick event).'), keywords: 'click klick detector maus', insert: (s) => s.insertObject('ClickDetector') },

  // ---- scripts
  { id: 'script', name: msg('Script'), category: 'scripts', icon: 'script', target: msg('Selected object or ServerScriptService'), description: msg('Empty script that runs when the game starts.'), keywords: 'script skript code lua', insert: (s) => s.insertObject('Script') },
  { id: 'localscript', name: msg('LocalScript'), category: 'scripts', icon: 'localscript', target: msg('Selected object or StarterPlayerScripts'), description: msg('Script for the local player (input, UI).'), keywords: 'local script lokal client', insert: (s) => s.insertObject('LocalScript') },
  { id: 'modulescript', name: msg('ModuleScript'), category: 'scripts', icon: 'modulescript', target: msg('Selected object or ReplicatedStorage'), description: msg('Reusable code loaded with require().'), keywords: 'module modul bibliothek require', insert: (s) => s.insertObject('ModuleScript') },
  ...([
    ['Kill Brick', 'part', msg('Kill on touch'), msg('Kills any character touching the selected part.'), 'kill töten touch'],
    ['Spinner', 'part', msg('Spin part'), msg('Rotates the selected part every frame.'), 'spin drehen rotate'],
    ['Jump Pad', 'part', msg('Launch on touch'), msg('Launches characters touching the part upward.'), 'jump launch sprung'],
    ['Moving Platform', 'part', msg('Move back and forth'), msg('Tweens the part 20 studs back and forth.'), 'move tween bewegen'],
    ['Click Door', 'part', msg('Click to open'), msg('Opens/closes the part when clicked.'), 'door click tür'],
    ['Raycast Laser', 'part', msg('Raycast sensor'), msg('Turns red while something is in front of the part.'), 'raycast laser sensor'],
    ['Coin Pickup', 'part', msg('Collectible'), msg('Destroys the part on touch and adds a coin.'), 'coin collect münze'],
    ['Day/Night Cycle', 'service', msg('Day/night cycle'), msg('Advances Lighting.ClockTime continuously.'), 'day night tag nacht zeit'],
    ['Button Counter (LocalScript)', 'button', msg('Button counter'), msg('Counts clicks on the selected TextButton.'), 'button gui zähler'],
  ] as [string, 'part' | 'service' | 'button', string, string, string][]).map(([tpl, kind, n, d, k]): ToolboxItem => ({
    id: 'tpl:' + tpl, name: n, category: 'scripts', icon: kind === 'button' ? 'localscript' : 'script',
    target: kind === 'part' ? 'Selected part' : kind === 'button' ? 'Selected TextButton' : 'ServerScriptService', needsPart: kind === 'part',
    description: d, keywords: k + ' script template vorlage', insert: (s) => s.insertScriptTemplate(tpl, kind),
  })),

  // ---- UI
  { id: 'screengui', name: msg('Screen GUI'), category: 'ui', icon: 'screengui', target: msg('StarterGui'), description: msg('Container for on-screen UI. Add labels and buttons inside.'), keywords: 'gui ui screen oberfläche hud', insert: (s) => s.insertObject('ScreenGui') },
  { id: 'glass-panel', name: msg('Glass Panel'), category: 'ui', icon: 'frame', target: msg('Selected ScreenGui (or a new one)'), description: msg('Frosted-glass frame with blur, rounded corners and a soft shadow.'), keywords: 'glass blur panel glas unschärfe modern', insert: (s) => s.insertStyledGui('glass') },
  { id: 'gradient-button', name: msg('Gradient Button'), category: 'ui', icon: 'textbutton', target: msg('Selected ScreenGui (or a new one)'), description: msg('Rounded button with a color gradient and outline.'), keywords: 'button gradient verlauf knopf modern', insert: (s) => s.insertStyledGui('gradient') },
  { id: 'hud-label', name: msg('HUD Counter'), category: 'ui', icon: 'textlabel', target: msg('Selected ScreenGui (or a new one)'), description: msg('Blurred pill-shaped label for scores or timers.'), keywords: 'hud counter score punkte anzeige', insert: (s) => s.insertStyledGui('hud') },
  { id: 'textlabel', name: msg('Text Label'), category: 'ui', icon: 'textlabel', target: msg('Selected ScreenGui (or a new one)'), description: msg('Displays text on screen.'), keywords: 'text label beschriftung anzeige', insert: (s) => s.insertObject('TextLabel') },
  { id: 'textbutton', name: msg('Button'), category: 'ui', icon: 'textbutton', target: msg('Selected ScreenGui (or a new one)'), description: msg('Clickable button (MouseButton1Click).'), keywords: 'button knopf schaltfläche', insert: (s) => s.insertObject('TextButton') },
  { id: 'frame', name: msg('Frame'), category: 'ui', icon: 'frame', target: msg('Selected ScreenGui (or a new one)'), description: msg('Rectangle / panel to group UI.'), keywords: 'frame rahmen panel box', insert: (s) => s.insertObject('Frame') },
  { id: 'image', name: msg('Image'), category: 'ui', icon: 'imagelabel', target: msg('Selected ScreenGui (or a new one)'), description: msg('Shows a texture asset.'), keywords: 'image bild grafik', insert: (s) => s.insertObject('ImageLabel') },

  // ---- environment
  { id: 'sky', name: msg('Sky'), category: 'environment', icon: 'sky', target: msg('Lighting'), description: msg('Sky colors, sun and stars.'), keywords: 'sky himmel sonne sterne', insert: (s) => s.insertObject('Sky') },
  { id: 'atmosphere', name: msg('Atmosphere'), category: 'environment', icon: 'atmosphere', target: msg('Lighting'), description: msg('Distance haze / fog.'), keywords: 'atmosphere fog nebel dunst', insert: (s) => s.insertObject('Atmosphere') },
  { id: 'camera', name: msg('Camera'), category: 'environment', icon: 'camera', target: msg('Workspace (at current view)'), description: msg('Fixed or scripted game camera.'), keywords: 'camera kamera ansicht', insert: (s) => s.insertObject('Camera') },
  { id: 'terrain', name: msg('Terrain Editor'), category: 'environment', icon: 'terrain', target: msg('Terrain'), description: msg('Sculpt hills, paint materials and add water.'), keywords: 'terrain gelände landschaft berg wasser', insert: (s) => s.setTool('terrain') },
  ...(['IntValue', 'NumberValue', 'StringValue', 'BoolValue'] as const).map((c): ToolboxItem => ({
    id: c, name: c.replace('Value', ' Value'), category: 'environment', icon: 'value', target: msg('Selected object or Workspace'),
    description: `Stores a ${c.replace('Value', '').toLowerCase()} value scripts can read and change.`, keywords: 'value wert variable ' + c.toLowerCase(), insert: (s) => s.insertObject(c),
  })),
];

export function toolboxItems(): ToolboxItem[] { return ITEMS; }
export function toolboxItem(id: string) { return ITEMS.find((i) => i.id === id); }

/** Fuzzy-ish search over name, description and keywords. */
export function searchToolbox(query: string, category: ToolboxCategory | 'all'): ToolboxItem[] {
  const q = query.trim().toLowerCase();
  const words = q.split(/\s+/).filter(Boolean);
  const list = ITEMS.filter((i) => category === 'all' || i.category === category);
  if (!words.length) return list;
  const scored = list.map((i) => {
    const name = `${i.name} ${t(i.name)}`.toLowerCase();
    const hay = `${name} ${i.keywords} ${i.description} ${t(i.description)}`.toLowerCase();
    let score = 0;
    for (const w of words) {
      if (name.startsWith(w) || t(i.name).toLowerCase().startsWith(w)) score += 5;
      else if (name.includes(w)) score += 3;
      else if (i.keywords.includes(w)) score += 2;
      else if (hay.includes(w)) score += 1;
      else return { i, score: 0 };
    }
    return { i, score };
  }).filter((x) => x.score > 0);
  return scored.sort((a, b) => b.score - a.score).map((x) => x.i);
}

const RECENT_KEY = 'forge.toolbox.recent';
export function recentToolbox(): ToolboxItem[] {
  try { return (JSON.parse(localStorage.getItem(RECENT_KEY) ?? '[]') as string[]).map(toolboxItem).filter((x): x is ToolboxItem => !!x); } catch { return []; }
}
export function markToolboxUsed(id: string) {
  try {
    const list = [id, ...(JSON.parse(localStorage.getItem(RECENT_KEY) ?? '[]') as string[]).filter((x) => x !== id)].slice(0, 8);
    localStorage.setItem(RECENT_KEY, JSON.stringify(list));
  } catch { /* storage unavailable */ }
}

export { SCRIPT_TEMPLATES };
