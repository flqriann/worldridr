# Language packs

Forge Studio and Forge Player are in **English** by default and include
**German** (Deutsch). Switch the language in **Settings ▸ Language** (Studio:
gear icon on the start screen or *File ▸ Settings…*; Player: language selector
in the top bar). The choice is remembered.

## Adding a language

1. **Settings ▸ Language ▸ Export template (English)** (or *(German)* if you
   prefer to translate from German). This saves a JSON file with every UI text.
2. Edit the file:

   ```json
   {
     "format": "forge-language-pack",
     "version": 1,
     "code": "fr",
     "name": "Français",
     "author": "Your name",
     "strings": {
       "Save": "Enregistrer",
       "Open {name}": "Ouvrir {name}",
       "…": "…"
     }
   }
   ```

   - `code`: language code such as `fr`, `es`, `pt-BR` (not `en` or `de`, which
     are built in – use e.g. `de-custom` to override German).
   - `name`: shown in the language list.
   - `strings`: the **keys are the English texts – do not change them**, only
     the values. Keep placeholders like `{name}`, `{n}`, `{error}` unchanged
     (you may move them inside the sentence).
   - Missing or empty entries fall back to English, so partial packs work. The
     list shows how complete each pack is (in %).
3. Install it:
   - **Settings ▸ Language ▸ Install language pack…** – stored with the app
     and selectable immediately (remove it there again), or
   - desktop app only: **Open languages folder** and copy the `.json` file there
     (`%APPDATA%/Forge/languages` on Windows, `~/.config/Forge/languages` on Linux – shared by Studio and Player). Packs in this folder are
     loaded at every start – handy for sharing a pack with a team.

Both Forge Studio and Forge Player use the same packs.

## Notes for developers

- Every UI text is written in English inside `t('…')` (or `msg('…')` for
  texts in data tables that are translated where they are displayed).
  Placeholders use `t('Saved {name}', { name })`.
- After adding texts, run `npm run i18n:check` – it lists texts missing from the
  German pack (`src/i18n/de.ts`) and keys that are no longer used. The exported
  templates are generated from the German pack's key list, so keep it complete.
- Lua error messages and script API names stay in English on purpose.
