// Forge Player package (game client): starts directly in player mode.
const base = require('./build/builder-base.cjs');

module.exports = base({
  id: 'player',
  name: 'Forge Player',
  mode: 'player',
  icon: 'build/icons/player.png',
  artifact: 'ForgePlayer',
});
