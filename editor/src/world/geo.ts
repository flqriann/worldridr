// Geo projection identical to the worldridr game (World::toLocal / toLatLon):
// local x = east, z = south, y = up; origin = (lat0, lon0); meters.

const EARTH = 40075016.686;
const PI = Math.PI;

export const lonToMx = (lon: number) => (lon + 180) / 360;
export const latToMy = (lat: number) => {
  const l = Math.max(-85.05, Math.min(85.05, lat));
  return (1 - Math.asinh(Math.tan((l * PI) / 180)) / PI) / 2;
};
export const myToLat = (my: number) => (Math.atan(Math.sinh(PI * (1 - 2 * my))) * 180) / PI;

export interface GeoRef { lat0: number; lon0: number; mx0: number; my0: number; S: number }

export function makeGeo(lat0: number, lon0: number): GeoRef {
  return { lat0, lon0, mx0: lonToMx(lon0), my0: latToMy(lat0), S: EARTH * Math.cos((lat0 * PI) / 180) };
}

export function toLocal(g: GeoRef, lat: number, lon: number): [number, number] {
  return [(lonToMx(lon) - g.mx0) * g.S, (latToMy(lat) - g.my0) * g.S];
}

export function toLatLon(g: GeoRef, x: number, z: number): [number, number] {
  return [myToLat(g.my0 + z / g.S), (g.mx0 + x / g.S) * 360 - 180];
}

/** Slippy-map tile of a coordinate. */
export function tileOf(lat: number, lon: number, zoom: number): [number, number] {
  const n = 2 ** zoom;
  const mx = Math.floor(lonToMx(lon) * n), my = Math.floor(latToMy(lat) * n);
  return [Math.max(0, Math.min(n - 1, mx)), Math.max(0, Math.min(n - 1, my))];
}
