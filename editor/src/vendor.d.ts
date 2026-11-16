declare module 'monaco-editor/editor/editor.main' {
  export * from 'monaco-editor';
}
declare module 'monaco-editor/editor/editor.worker?worker' {
  const WorkerCtor: { new (): Worker };
  export default WorkerCtor;
}
