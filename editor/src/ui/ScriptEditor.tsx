// Script editor (Monaco): one model per script, synced with the Source
// property (undoable), live syntax-error markers, runtime-error markers,
// go-to-line from the Output, format, find/replace (Monaco built-in).

import { useEffect, useRef } from 'react';
import type { Studio } from '../editor/Studio';
import type { Document } from '../editor/Document';
import type { Instance } from '../core/Instance';
import { monaco, sceneNames } from './monacoSetup';
import { LuaRuntime } from '../runtime/LuaRuntime';

import { t } from '../i18n';
const models = new Map<string, monaco.editor.ITextModel>();

function modelFor(inst: Instance): monaco.editor.ITextModel {
  let m = models.get(inst.id);
  if (!m || m.isDisposed()) {
    m = monaco.editor.createModel(inst.props.Source as string, 'lua', monaco.Uri.parse(`forge://script/${inst.id}.lua`));
    models.set(inst.id, m);
  }
  return m;
}

export function ScriptEditor({ studio, doc, scriptId }: { studio: Studio; doc: Document; scriptId: string }) {
  const host = useRef<HTMLDivElement>(null);
  const editor = useRef<monaco.editor.IStandaloneCodeEditor | null>(null);
  const inst = doc.dm.byId(scriptId);

  useEffect(() => {
    if (!host.current) return;
    const ed = monaco.editor.create(host.current, {
      theme: 'forge-dark', automaticLayout: true, fontSize: 14, fontFamily: 'Cascadia Code, Consolas, monospace', tabSize: 4, insertSpaces: false,
      minimap: { enabled: true, scale: 1 }, scrollBeyondLastLine: false, renderWhitespace: 'selection', bracketPairColorization: { enabled: true },
      fixedOverflowWidgets: true,
    });
    editor.current = ed;
    ed.addCommand(monaco.KeyMod.Shift | monaco.KeyMod.Alt | monaco.KeyCode.KeyF, () => ed.getAction('editor.action.formatDocument')?.run());
    return () => { ed.dispose(); editor.current = null; };
  }, [studio]);

  // bind the model of the current script and sync both ways
  useEffect(() => {
    const ed = editor.current;
    if (!ed || !inst) return;
    const model = modelFor(inst);
    if (model.getValue() !== inst.props.Source) model.setValue(inst.props.Source as string);
    ed.setModel(model);
    ed.focus();
    sceneNames.list = [...new Set(doc.dm.getDescendants().map((i) => i.name))].filter((n) => /^[A-Za-z_]\w*$/.test(n));

    let applying = false;
    let timer: number | undefined;
    let checkTimer: number | undefined;
    const check = () => {
      LuaRuntime.checkSyntax(model.getValue()).then((err) => {
        if (model.isDisposed()) return;
        const markers = err ? [{ severity: monaco.MarkerSeverity.Error, message: err.message, startLineNumber: err.line, endLineNumber: err.line, startColumn: 1, endColumn: model.getLineMaxColumn(Math.min(err.line, model.getLineCount())) }] : [];
        monaco.editor.setModelMarkers(model, 'lua-syntax', markers);
      }).catch(() => {});
    };
    check();
    const sub = model.onDidChangeContent(() => {
      if (applying) return;
      monaco.editor.setModelMarkers(model, 'lua-runtime', []);
      clearTimeout(timer);
      timer = window.setTimeout(() => {
        const v = model.getValue();
        if (v !== inst.props.Source && inst.dm) doc.transact(`Edit ${inst.name}`, () => inst.set('Source', v), `src:${inst.id}`);
      }, 250);
      clearTimeout(checkTimer);
      checkTimer = window.setTimeout(check, 400);
    });
    // external changes (undo/redo, command bar) update the editor without losing its undo stack
    const off = doc.dm.changed.connect((i, prop) => {
      if (i !== inst || prop !== 'Source') return;
      const v = inst.props.Source as string;
      if (model.getValue() === v) return;
      applying = true;
      model.pushEditOperations([], [{ range: model.getFullModelRange(), text: v }], () => null);
      applying = false;
      check();
    });
    // runtime errors from the Output mark the line
    const offLog = studio.output.changed.connect(() => {
      const last = studio.output.entries.at(-1);
      if (last?.level === 'error' && last.source?.scriptId === inst.id && last.source.line) {
        const ln = Math.min(last.source.line, model.getLineCount());
        monaco.editor.setModelMarkers(model, 'lua-runtime', [{ severity: monaco.MarkerSeverity.Error, message: last.message.split('\n')[0], startLineNumber: ln, endLineNumber: ln, startColumn: 1, endColumn: model.getLineMaxColumn(ln) }]);
      }
    });
    const offGoto = studio.panelRequest.connect((req) => {
      const m = req.match(/^goto:(.+):(\d+)$/);
      if (!m || m[1] !== inst.id) return;
      const ln = Math.min(+m[2], model.getLineCount());
      requestAnimationFrame(() => { ed.revealLineInCenter(ln); ed.setPosition({ lineNumber: ln, column: 1 }); ed.focus(); });
    });
    return () => {
      // flush pending edit
      clearTimeout(timer); clearTimeout(checkTimer);
      const v = model.getValue();
      if (inst.dm && v !== inst.props.Source) doc.transact(`Edit ${inst.name}`, () => inst.set('Source', v), `src:${inst.id}`);
      sub.dispose(); off(); offLog(); offGoto();
    };
  }, [inst, doc, studio]);

  if (!inst) return <div className="panel-empty">{t('Script not found.')}</div>;
  return <div className="script-editor" ref={host} />;
}

export function disposeScriptModel(id: string) { models.get(id)?.dispose(); models.delete(id); }

/** Runs a Monaco editor action on the visible script editor. */
export function runEditorAction(action: 'find' | 'replace' | 'format') {
  const ed = monaco.editor.getEditors().find((e) => e.hasTextFocus() || e.getDomNode()?.offsetParent) as monaco.editor.ICodeEditor | undefined;
  if (!ed) return false;
  const id = action === 'find' ? 'actions.find' : action === 'replace' ? 'editor.action.startFindReplaceAction' : 'editor.action.formatDocument';
  ed.focus();
  ed.getAction(id)?.run();
  return true;
}
