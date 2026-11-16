// OpenStreetMap access: place search (Nominatim) and the Overpass query for
// buildings, roads and water around a point, plus tag interpretation.

import { httpJson } from './net';

export interface PlaceResult { name: string; lat: number; lon: number; kind: string }

export async function searchPlace(query: string): Promise<PlaceResult[]> {
  const q = query.trim();
  // "48.2082, 16.3738" -> coordinates directly
  const m = q.match(/^\s*(-?\d+(?:[.,]\d+)?)\s*[,;\s]\s*(-?\d+(?:[.,]\d+)?)\s*$/);
  if (m) {
    const lat = parseFloat(m[1].replace(',', '.')), lon = parseFloat(m[2].replace(',', '.'));
    if (Math.abs(lat) <= 90 && Math.abs(lon) <= 180) return [{ name: `${lat.toFixed(5)}, ${lon.toFixed(5)}`, lat, lon, kind: 'coordinates' }];
  }
  const res = await httpJson<{ display_name: string; lat: string; lon: string; type?: string; class?: string }[]>(
    `https://nominatim.openstreetmap.org/search?format=json&limit=6&addressdetails=0&q=${encodeURIComponent(q)}`);
  return res.map((r) => ({ name: r.display_name, lat: parseFloat(r.lat), lon: parseFloat(r.lon), kind: `${r.class ?? ''}/${r.type ?? ''}` }));
}

export interface OsmNode { lat: number; lon: number }
export interface OsmWay { id: number; tags: Record<string, string>; geometry: OsmNode[] }
export interface OsmData { ways: OsmWay[]; pois: { lat: number; lon: number; tags: Record<string, string> }[] }

const ENDPOINTS = ['https://overpass-api.de/api/interpreter', 'https://overpass.kumi.systems/api/interpreter', 'https://overpass.private.coffee/api/interpreter'];

export interface OsmWanted { buildings: boolean; roads: boolean; water: boolean; areas: boolean }

/** Metric half-size box around the center -> lat/lon bbox string "s,w,n,e". */
export function bboxAround(lat: number, lon: number, radius: number): string {
  const dLat = radius / 111320;
  const dLon = radius / (111320 * Math.cos((lat * Math.PI) / 180));
  return `${lat - dLat},${lon - dLon},${lat + dLat},${lon + dLon}`;
}

export async function fetchOsm(lat: number, lon: number, radius: number, want: OsmWanted, onStatus: (s: string) => void): Promise<OsmData> {
  const bb = bboxAround(lat, lon, radius);
  const parts: string[] = [];
  if (want.buildings) parts.push(`way["building"](${bb});`);
  if (want.roads) parts.push(`way["highway"]["highway"!~"^(proposed|construction|bus_stop|platform|corridor|elevator|steps|footway|path|cycleway|pedestrian|bridleway)$"](${bb});`);
  if (want.water) parts.push(`way["natural"="water"](${bb});`);
  if (want.areas) parts.push(`way["landuse"~"^(forest|grass|meadow|recreation_ground|village_green|cemetery)$"](${bb});`, `way["leisure"~"^(park|garden|pitch)$"](${bb});`);
  if (!parts.length) return { ways: [], pois: [] };
  const ql = `[out:json][timeout:60];(${parts.join('')});out tags geom qt;`;
  let lastErr: unknown = null;
  for (const ep of ENDPOINTS) {
    try {
      onStatus(`Loading OpenStreetMap data (${new URL(ep).hostname})…`);
      const j = await httpJson<{ elements: { type: string; id: number; tags?: Record<string, string>; geometry?: OsmNode[] }[] }>(ep, 'data=' + encodeURIComponent(ql));
      const ways: OsmWay[] = [];
      for (const e of j.elements) if (e.type === 'way' && e.geometry && e.geometry.length >= 2) ways.push({ id: e.id, tags: e.tags ?? {}, geometry: e.geometry });
      return { ways, pois: [] };
    } catch (err) { lastErr = err; }
  }
  throw new Error('OpenStreetMap (Overpass) is not reachable: ' + (lastErr instanceof Error ? lastErr.message : String(lastErr)));
}

// ---- tag interpretation ------------------------------------------------------------

const NAMED_COLORS: Record<string, string> = {
  white: '#f0eee8', grey: '#aaaaaa', gray: '#aaaaaa', black: '#333333', red: '#b04a3a', brown: '#8a6a4a', beige: '#d8c8a8', yellow: '#e6d48a',
  orange: '#d99a5a', green: '#7e9a6e', blue: '#6f8fb0', cream: '#eadfc2', pink: '#d9a8a8', tan: '#c8b08a', lightgrey: '#c8c8c8', darkgrey: '#777777',
};
export function osmColor(v: string | undefined): string | null {
  if (!v) return null;
  const s = v.trim().toLowerCase();
  if (/^#[0-9a-f]{6}$/.test(s)) return s;
  if (/^#[0-9a-f]{3}$/.test(s)) return '#' + [...s.slice(1)].map((c) => c + c).join('');
  return NAMED_COLORS[s] ?? null;
}

const MATERIAL_COLORS: Record<string, string> = {
  brick: '#b0715a', concrete: '#bdbab2', stone: '#c2b8a3', glass: '#9ab6c4', wood: '#a07e56', metal: '#a6adb3', plaster: '#e6dfcf', roof_tiles: '#a85a42',
  tile: '#a85a42', slate: '#5f6670', copper: '#6fa58f', tar_paper: '#4a4a4a', thatch: '#9a8450', eternit: '#8a8a86', grass: '#6f9a58',
};

function num(v: string | undefined): number | null {
  if (!v) return null;
  const n = parseFloat(v.replace(',', '.'));
  return Number.isFinite(n) ? n : null;
}

export interface BuildingTags { height: number; roofHeight: number; roofShape: string; wall: string; roof: string; levels: number; name: string }

/** Height, roof and colors from OSM tags (with sensible guesses where tags are missing). */
export function interpretBuilding(tags: Record<string, string>, areaM2: number, rnd: number): BuildingTags {
  const levels = num(tags['building:levels']) ?? 0;
  const minH = num(tags['min_height']) ?? 0;
  const roofLevels = num(tags['roof:levels']) ?? 0;
  const type = tags.building ?? 'yes';
  let height = num(tags.height);
  if (height === null) {
    if (levels > 0) height = levels * 3.1 + 1 + roofLevels * 2.2;
    else if (['garage', 'garages', 'shed', 'hut', 'carport', 'roof', 'service', 'kiosk', 'cabin'].includes(type)) height = 3 + rnd * 0.8;
    else if (['house', 'detached', 'semidetached_house', 'terrace', 'bungalow'].includes(type)) height = 6 + rnd * 2.5;
    else if (['apartments', 'residential', 'dormitory'].includes(type)) height = 11 + rnd * 8;
    else if (['church', 'cathedral', 'chapel'].includes(type)) height = 14 + rnd * 8;
    else if (['industrial', 'warehouse', 'retail', 'commercial', 'supermarket', 'hangar'].includes(type)) height = areaM2 > 600 ? 8 + rnd * 3 : 6 + rnd * 2;
    else height = areaM2 < 40 ? 3.5 : areaM2 < 150 ? 7 + rnd * 2.5 : 9 + rnd * 6;
  }
  height = Math.max(2.5, height - minH);
  let roofShape = ({ gabled: 'Gabled', hipped: 'Pyramid', pyramidal: 'Pyramid', skillion: 'Skillion', flat: 'Flat', dome: 'Pyramid', round: 'Flat' } as Record<string, string>)[tags['roof:shape'] ?? ''];
  if (!roofShape) {
    const pitched = ['house', 'detached', 'semidetached_house', 'terrace', 'residential', 'church', 'chapel', 'farm', 'barn', 'cabin', 'bungalow'].includes(type) || (areaM2 < 260 && height < 14 && type === 'yes' && rnd > 0.35);
    roofShape = pitched ? (rnd > 0.6 ? 'Pyramid' : 'Gabled') : 'Flat';
  }
  let roofHeight = num(tags['roof:height']) ?? 0;
  if (!roofHeight && roofShape !== 'Flat') roofHeight = Math.min(height * 0.5, 2 + Math.sqrt(areaM2) * 0.12);
  if (roofShape === 'Flat') roofHeight = 0;
  const wall = osmColor(tags['building:colour']) ?? MATERIAL_COLORS[tags['building:material'] ?? ''] ?? WALL_PALETTE[Math.floor(rnd * 997) % WALL_PALETTE.length];
  const roof = osmColor(tags['roof:colour']) ?? MATERIAL_COLORS[tags['roof:material'] ?? ''] ?? (roofShape === 'Flat' ? '#8d8d8b' : ROOF_PALETTE[Math.floor(rnd * 991) % ROOF_PALETTE.length]);
  return { height, roofHeight: Math.min(roofHeight, height * 0.9), roofShape, wall, roof, levels: Math.round(levels), name: tags.name ?? '' };
}

const WALL_PALETTE = ['#e6dfcf', '#d9cfbb', '#cfc6b8', '#e3d6b8', '#d4c9c0', '#ddd6cc', '#c9b99d', '#e8e2d4'];
const ROOF_PALETTE = ['#a85a42', '#8f4a3a', '#7a4a3a', '#5f5a58', '#6e5a50', '#9a5a46'];

/** worldridr road type for a highway tag. */
export function roadTypeFor(tags: Record<string, string>): string | null {
  const h = tags.highway;
  if (!h) return null;
  const oneway = tags.oneway === 'yes' || tags.oneway === '1';
  switch (h) {
    case 'motorway': case 'motorway_link': case 'trunk': case 'trunk_link': return 'Autobahn';
    case 'primary': case 'primary_link': case 'secondary': case 'secondary_link': return (num(tags.lanes) ?? 2) >= 3 ? '4 Spuren' : tags['maxspeed'] && (num(tags.maxspeed) ?? 50) >= 80 ? 'Landstraße' : 'Stadtstraße';
    case 'tertiary': case 'tertiary_link': case 'unclassified': return tags['maxspeed'] && (num(tags.maxspeed) ?? 50) >= 80 ? 'Landstraße' : oneway ? 'Einbahn' : 'Stadtstraße';
    case 'residential': case 'living_street': case 'service': case 'road': return oneway ? 'Einbahn' : 'Stadtstraße';
    case 'track': return 'Feldweg';
    default: return null;
  }
}
