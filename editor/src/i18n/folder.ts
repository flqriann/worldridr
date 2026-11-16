// Desktop: language packs dropped into <userData>/languages/*.json are loaded at startup.
import { native } from '../project/ProjectIO';
import { languages, parseLanguagePack, registerLanguagePack } from './index';

export async function loadFolderLanguagePacks(): Promise<string[]> {
  if (!native) return [];
  const errors: string[] = [];
  try {
    for (const text of await native.languageList()) {
      try {
        const pack = parseLanguagePack(text);
        if (!languages().some((l) => l.builtin && l.code === pack.code)) registerLanguagePack(pack);
      } catch (e) { errors.push((e as Error).message); }
    }
  } catch (e) { errors.push((e as Error).message); }
  return errors;
}
