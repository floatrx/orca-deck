// orca-deck spike (THROWAWAY): asks the Orca desktop to show a workspace or one agent's tab, through
// the runtime RPCs the orca CLI itself uses. Runs under Orca's own Electron in node mode, so Orca's
// runtime client and its dependencies resolve exactly as they do for the CLI:
//
//   ELECTRON_RUN_AS_NODE=1 /Applications/Orca.app/Contents/MacOS/Orca reveal.cjs pane <worktreeId> <tabId> [leafId]
//   ELECTRON_RUN_AS_NODE=1 /Applications/Orca.app/Contents/MacOS/Orca reveal.cjs worktree <worktreeId>
//
// `pane` calls terminal.reveal (one tab, chat or terminal); `worktree` calls worktree.activate (the
// workspace as it was last left — also for a workspace with no agent). Prints the runtime's answer as
// one JSON line; {"error": …} and exit code 1 on failure.
const OUT = "/Applications/Orca.app/Contents/Resources/app.asar.unpacked/out";
// Orca moves its internals between releases (1.4.259: cli/runtime-client.js → cli/runtime/{client,metadata}.js).
function requireFirst(paths) {
  for (const path of paths) {
    try {
      return require(OUT + path);
    } catch (error) {
      if (error.code !== "MODULE_NOT_FOUND") throw error;
    }
  }
  throw new Error("Orca module not found: " + paths.join(" | "));
}
const { RuntimeClient } = requireFirst(["/cli/runtime/client.js", "/cli/runtime-client.js"]);
const { getDefaultUserDataPath } = requireFirst(["/cli/runtime/metadata.js", "/cli/runtime-client.js"]);

const [mode, worktreeId, tabId, leafId] = process.argv.slice(2);

function request() {
  if (mode === "worktree") return ["worktree.activate", { worktree: `id:${worktreeId}`, navigation: "host" }];
  if (mode === "pane") return ["terminal.reveal", { worktreeId, tabId, ...(leafId ? { leafId } : {}) }];
  throw new Error(`unknown mode "${mode}": use pane or worktree`);
}

(async () => {
  const [method, params] = request();
  const client = new RuntimeClient(getDefaultUserDataPath(), undefined, null, null);
  const answer = await client.call(method, params);
  process.stdout.write(JSON.stringify(answer.result ?? answer) + "\n");
})().catch((error) => {
  process.stdout.write(JSON.stringify({ error: String(error?.message ?? error) }) + "\n");
  process.exit(1);
});
