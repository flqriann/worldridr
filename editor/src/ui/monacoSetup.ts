// Monaco configuration: worker, dark theme, Lua completions for the Forge
// scripting API, and a document formatter for Lua.

import * as monaco from 'monaco-editor/editor/editor.main';
import EditorWorker from 'monaco-editor/editor/editor.worker?worker';
import { allClasses, classEvents, classMethods, getProps } from '../core/classes';
import { ENUMS } from '../runtime/enums';

(self as unknown as { MonacoEnvironment: unknown }).MonacoEnvironment = { getWorker: () => new EditorWorker() };

monaco.editor.defineTheme('forge-dark', {
  base: 'vs-dark', inherit: true,
  rules: [
    { token: 'keyword', foreground: 'f86d7c' },
    { token: 'string', foreground: 'adf195' },
    { token: 'number', foreground: 'ffc600' },
    { token: 'comment', foreground: '666f80', fontStyle: 'italic' },
    { token: 'identifier', foreground: 'e6e8ec' },
  ],
  colors: { 'editor.background': '#1e1f23', 'editor.lineHighlightBackground': '#2a2c32', 'editorLineNumber.foreground': '#5b606b', 'editorGutter.background': '#1e1f23' },
});

const GLOBALS: [string, string][] = [
  ['game', 'The DataModel (root of the scene)'], ['workspace', 'The Workspace service'], ['script', 'The running script'],
  ['Instance', 'Instance.new(className, parent?)'], ['Vector3', 'Vector3.new(x, y, z)'], ['Vector2', 'Vector2.new(x, y)'], ['Color3', 'Color3.new / fromRGB / fromHSV'],
  ['CFrame', 'CFrame.new / Angles / lookAt'], ['UDim2', 'UDim2.new / fromScale / fromOffset'], ['UDim', 'UDim.new(scale, offset)'], ['Enum', 'Enumerations'],
  ['TweenInfo', 'TweenInfo.new(time, style, direction, repeat, reverses, delay)'], ['RaycastParams', 'RaycastParams.new()'], ['task', 'Task scheduler'],
  ['wait', 'wait(seconds) – yields'], ['delay', 'delay(seconds, fn)'], ['spawn', 'spawn(fn)'], ['print', 'Print to Output'], ['warn', 'Warning to Output'],
  ['error', 'Raise an error'], ['typeof', 'Roblox-style type name'], ['require', 'require(ModuleScript)'], ['tick', 'Seconds (wall clock)'], ['time', 'Seconds since the test started'],
  ['pairs', ''], ['ipairs', ''], ['pcall', ''], ['tostring', ''], ['tonumber', ''], ['type', ''], ['select', ''], ['math', ''], ['string', ''], ['table', ''], ['coroutine', ''], ['os', ''], ['shared', 'Shared table'],
];
const MEMBERS: Record<string, string[]> = {
  Vector3: ['new', 'zero', 'one', 'xAxis', 'yAxis', 'zAxis'], Vector2: ['new', 'zero', 'one'], Color3: ['new', 'fromRGB', 'fromHSV', 'fromHex'],
  CFrame: ['new', 'Angles', 'lookAt', 'fromEulerAnglesYXZ', 'fromEulerAnglesXYZ', 'fromOrientation', 'fromAxisAngle', 'identity'],
  UDim2: ['new', 'fromScale', 'fromOffset'], UDim: ['new'], Instance: ['new'], TweenInfo: ['new'], RaycastParams: ['new'],
  task: ['wait', 'spawn', 'defer', 'delay', 'cancel'],
  math: ['abs', 'ceil', 'floor', 'max', 'min', 'random', 'randomseed', 'sqrt', 'sin', 'cos', 'tan', 'atan', 'asin', 'acos', 'pi', 'huge', 'clamp', 'fmod', 'modf', 'exp', 'log', 'tointeger', 'type'],
  string: ['format', 'sub', 'len', 'lower', 'upper', 'rep', 'reverse', 'find', 'match', 'gmatch', 'gsub', 'byte', 'char', 'split'],
  table: ['insert', 'remove', 'concat', 'sort', 'unpack', 'pack', 'move'],
  coroutine: ['create', 'resume', 'yield', 'status', 'wrap', 'running', 'close'],
};
const SERVICES = ['Workspace', 'Players', 'Lighting', 'ReplicatedStorage', 'ServerStorage', 'ServerScriptService', 'StarterGui', 'StarterPlayer', 'SoundService', 'RunService', 'UserInputService', 'TweenService', 'Debris'];
const SNIPPETS: [string, string, string][] = [
  ['function', 'function ${1:name}(${2})\n\t$0\nend', 'Function'],
  ['if', 'if ${1:condition} then\n\t$0\nend', 'If statement'],
  ['for', 'for ${1:i} = ${2:1}, ${3:10} do\n\t$0\nend', 'Numeric for'],
  ['forp', 'for ${1:key}, ${2:value} in pairs(${3:t}) do\n\t$0\nend', 'for pairs'],
  ['fori', 'for ${1:index}, ${2:value} in ipairs(${3:t}) do\n\t$0\nend', 'for ipairs'],
  ['while', 'while ${1:true} do\n\t$0\n\ttask.wait()\nend', 'While loop'],
  ['local', 'local ${1:name} = ${0}', 'Local variable'],
  ['touched', '${1:part}.Touched:Connect(function(hit)\n\tlocal humanoid = hit.Parent:FindFirstChild("Humanoid")\n\tif humanoid then\n\t\t$0\n\tend\nend)', 'Touched handler'],
  ['getservice', 'local ${1:Players} = game:GetService("${1:Players}")', 'GetService'],
];

let propNames: string[] = [];
let methodNames: string[] = [];
let eventNames: string[] = [];
{
  const p = new Set<string>(), m = new Set<string>(), e = new Set<string>();
  for (const c of allClasses()) {
    if (!c.abstract) for (const d of getProps(c.name)) if (!d.hidden) p.add(d.name);
    for (const x of classMethods(c.name)) m.add(x);
    for (const x of classEvents(c.name)) e.add(x);
  }
  ['Parent', 'CFrame', 'CurrentCamera', 'Character', 'RespawnLocation', 'LocalPlayer', 'PrimaryPart', 'Position', 'LookVector', 'RightVector', 'UpVector', 'Magnitude', 'Unit', 'X', 'Y', 'Z'].forEach((x) => p.add(x));
  ['Connect', 'Once', 'Wait', 'Disconnect', 'Play', 'Pause', 'Cancel', 'Lerp', 'Dot', 'Cross', 'Inverse', 'ToEulerAnglesYXZ', 'PointToWorldSpace', 'PointToObjectSpace'].forEach((x) => m.add(x));
  propNames = [...p]; methodNames = [...m]; eventNames = [...e];
}

/** Names of instances in the open scene, supplied by the editor for `workspace.` completion. */
export const sceneNames = { list: [] as string[] };

monaco.languages.registerCompletionItemProvider('lua', {
  triggerCharacters: ['.', ':', '"'],
  provideCompletionItems(model, position) {
    const line = model.getLineContent(position.lineNumber).slice(0, position.column - 1);
    const word = model.getWordUntilPosition(position);
    const range = { startLineNumber: position.lineNumber, endLineNumber: position.lineNumber, startColumn: word.startColumn, endColumn: word.endColumn };
    const K = monaco.languages.CompletionItemKind;
    const item = (label: string, kind: monaco.languages.CompletionItemKind, detail = '', insert?: string, snippet = false) =>
      ({ label, kind, detail, insertText: insert ?? label, range, insertTextRules: snippet ? monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet : undefined });

    let m = line.match(/GetService\(\s*["']\w*$/);
    if (m) return { suggestions: SERVICES.map((s) => item(s, K.Module, 'Service')) };
    m = line.match(/Enum\.(\w+)\.\w*$/);
    if (m && ENUMS[m[1]]) return { suggestions: ENUMS[m[1]].map((n) => item(n, K.EnumMember, `Enum.${m![1]}`)) };
    if (/Enum\.\w*$/.test(line)) return { suggestions: Object.keys(ENUMS).map((n) => item(n, K.Enum, 'Enum type')) };
    m = line.match(/\b(\w+)\.\w*$/);
    if (m && MEMBERS[m[1]]) return { suggestions: MEMBERS[m[1]].map((n) => item(n, K.Function, m![1])) };
    if (/:\w*$/.test(line)) return { suggestions: methodNames.map((n) => item(n, K.Method, 'method')) };
    if (/\.\w*$/.test(line)) {
      return { suggestions: [
        ...propNames.map((n) => item(n, K.Property, 'property')),
        ...eventNames.map((n) => item(n, K.Event, 'event')),
        ...sceneNames.list.map((n) => item(n, K.Field, 'child')),
      ] };
    }
    return { suggestions: [
      ...GLOBALS.map(([n, d]) => item(n, K.Variable, d)),
      ...SNIPPETS.map(([n, body, d]) => item(n, K.Snippet, d, body, true)),
      ...['local', 'function', 'end', 'then', 'else', 'elseif', 'return', 'and', 'or', 'not', 'nil', 'true', 'false', 'break', 'repeat', 'until', 'in', 'do'].map((k) => item(k, K.Keyword, 'keyword')),
    ] };
  },
});

/** Re-indents Lua code based on block keywords. */
export function formatLua(src: string, indent = '\t'): string {
  const out: string[] = [];
  let level = 0;
  let inLongString = false;
  for (const raw of src.split('\n')) {
    const line = raw.trim();
    if (inLongString) { out.push(raw); if (raw.includes(']]')) inLongString = false; continue; }
    const code = line.replace(/--.*$/, '').replace(/"(\\.|[^"\\])*"|'(\\.|[^'\\])*'/g, '""').replace(/\[\[.*?\]\]/g, '""');
    if (/\[\[/.test(code) && !/\]\]/.test(code)) inLongString = true;
    const startsClose = /^(end\b|until\b|else\b|elseif\b|[}\)])/.test(code);
    if (startsClose) level = Math.max(0, level - 1);
    out.push(line ? indent.repeat(level) + line : '');
    const opens = (code.match(/\b(function|then|do|repeat|else)\b/g)?.length ?? 0) + (code.match(/[({]/g)?.length ?? 0);
    const closes = (code.match(/\b(end|until|elseif)\b/g)?.length ?? 0) + (code.match(/[)}]/g)?.length ?? 0);
    // a leading closer was already applied above, so don't count it twice
    const compensated = /^(end\b|until\b|elseif\b|[})])/.test(code);
    const net = opens - closes + (compensated ? 1 : 0);
    // one line changes the indentation by at most one level, so `foo(function()` / `end)` indent once
    level = Math.max(0, level + Math.max(startsClose ? 0 : -1, Math.min(1, net)));
  }
  return out.join('\n');
}

monaco.languages.registerDocumentFormattingEditProvider('lua', {
  provideDocumentFormattingEdits(model) {
    return [{ range: model.getFullModelRange(), text: formatLua(model.getValue()) }];
  },
});

export { monaco };
