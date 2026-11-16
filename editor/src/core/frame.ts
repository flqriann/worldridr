/**
 * Runs `fn` on the next animation frame, or after a short timeout when the
 * window is hidden (browsers pause requestAnimationFrame for hidden pages,
 * which would otherwise freeze UI updates until the window is shown again).
 */
export function nextFrame(fn: () => void) {
  if (typeof document !== 'undefined' && document.hidden) setTimeout(fn, 50);
  else requestAnimationFrame(fn);
}
