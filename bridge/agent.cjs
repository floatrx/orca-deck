// orca-deck bridge: reads and writes an Orca chat agent session through the runtime RPCs the Orca
// clients use. Runs under Orca's own Electron in node mode, so Orca's runtime client and its helpers
// resolve exactly as they do for the CLI (same pattern as reveal.cjs):
//
//   ELECTRON_RUN_AS_NODE=1 /Applications/Orca.app/Contents/MacOS/Orca agent.cjs history <sessionId>
//   ELECTRON_RUN_AS_NODE=1 /Applications/Orca.app/Contents/MacOS/Orca agent.cjs say <sessionId>   (text on stdin)
//   ELECTRON_RUN_AS_NODE=1 /Applications/Orca.app/Contents/MacOS/Orca agent.cjs answer <sessionId> (JSON on stdin)
//
// history → {"message": <newest assistant text>, "working": bool, "fence": n, "question"?: {…}} — `question` is
//           the newest pending question the user can act on (an AskUserQuestion), as Orca's renderer finds it.
// say     → {"sent": true, "dispatch": "pending"|…} — the message is built and fingerprinted by Orca's own
//           structuredAgentSessionSendBody / structuredAgentSessionMessageSendMutation, fenced at the
//           session's current runtime fence.
// answer  → {"answered": true} — stdin {"itemId", "revision", "optionId"}; agentSession.respondToQuestion with
//           the envelope Orca's renderer builds (fingerprint method "agentSession.respondTo:question").
// Every failure prints {"error": …} and exits 1. The text is read from stdin so it never shows up in `ps`.
const OUT = "/Applications/Orca.app/Contents/Resources/app.asar.unpacked/out";
const { RuntimeClient, getDefaultUserDataPath } = require(OUT + "/cli/runtime-client.js");
const send = require(OUT + "/shared/structured-agent-session-send-mutation.js");
const mutation = require(OUT + "/shared/structured-agent-session-mutation.js");
const { randomBytes } = require("node:crypto");

const [mode, sessionId] = process.argv.slice(2);
const HISTORY_LIMIT = 40;

function textOf(body) {
  return (body.blocks || []).filter((block) => block.type === "text").map((block) => block.text).join("").trim();
}

async function tail(client) {
  const answer = await client.call("agentSession.history", { sessionId, direction: "tail", limit: HISTORY_LIMIT });
  const page = (answer.result ?? answer).page;
  if (!page) throw new Error("no history page: " + JSON.stringify(answer).slice(0, 200));
  return page;
}

function newestAssistantText(page) {
  const items = [...(page.items || [])].sort((a, b) => (a.sequence ?? 0) - (b.sequence ?? 0));
  for (let i = items.length - 1; i >= 0; i--) {
    const body = items[i].body || {};
    if (body.kind === "message" && body.role === "assistant") {
      const text = textOf(body);
      if (text) return text;
    }
  }
  return "";
}

// The newest question the user can still answer: pending, and actionable per the page (the renderer's filter).
// `answerable` is false when the watch cannot answer it with one tap: several questions, multi-select, or no
// options (free text only).
function pendingQuestion(page) {
  const actionable = new Set(page.actionablePromptIds || []);
  const items = [...(page.items || [])].sort((a, b) => (a.sequence ?? 0) - (b.sequence ?? 0));
  for (let i = items.length - 1; i >= 0; i--) {
    const item = items[i];
    const body = item.body || {};
    if (body.kind !== "question" || body.resolution?.state !== "pending" || !actionable.has(item.itemId)) continue;
    const questions = body.questions || [{ question: body.question, options: body.options, multiSelect: false }];
    const first = questions[0] || {};
    const options = (first.options || body.options || []).map((option) => ({ id: option.id, label: option.label }));
    return {
      itemId: item.itemId,
      revision: item.revision,
      text: first.question || body.question || "",
      ...(first.header ? { header: first.header } : {}),
      options,
      answerable: questions.length === 1 && first.multiSelect !== true && options.length > 0,
    };
  }
  return null;
}

function readStdin() {
  return new Promise((resolve) => {
    let data = "";
    process.stdin.setEncoding("utf8");
    process.stdin.on("data", (chunk) => (data += chunk));
    process.stdin.on("end", () => resolve(data));
  });
}

(async () => {
  if (!sessionId) throw new Error("usage: agent.cjs history|say|answer <sessionId>");
  const client = new RuntimeClient(getDefaultUserDataPath(), undefined, null, null);
  if (mode === "history") {
    const page = await tail(client);
    const question = pendingQuestion(page);
    process.stdout.write(JSON.stringify({
      message: newestAssistantText(page), working: page.working === true, fence: page.fence,
      ...(question ? { question } : {}),
    }) + "\n");
    return;
  }
  if (mode === "answer") {
    const request = JSON.parse((await readStdin()) || "{}");
    if (typeof request.itemId !== "string" || typeof request.optionId !== "string" || !Number.isInteger(request.revision)) {
      throw new Error("answer needs itemId, revision and optionId");
    }
    const page = await tail(client);
    const fields = { itemId: request.itemId, expectedRevision: request.revision, optionId: request.optionId };
    const envelope = {
      sessionId,
      clientOperationId: `${Date.now()}-${randomBytes(16).toString("hex")}`,
      expectedRuntimeFence: page.fence ?? null,
      payloadFingerprint: mutation.structuredAgentSessionPayloadFingerprint({
        method: "agentSession.respondTo:question", sessionId, fields,
      }),
    };
    const answer = (await client.call("agentSession.respondToQuestion", { envelope, ...fields })).result;
    if (!answer || answer.ok !== true) {
      throw new Error(answer?.refusal?.details?.reason || answer?.refusal?.code || "answer refused");
    }
    process.stdout.write(JSON.stringify({ answered: true }) + "\n");
    return;
  }
  if (mode === "say") {
    const text = (await readStdin()).trim();
    if (!text) throw new Error("empty text");
    const page = await tail(client);
    const mutation = send.structuredAgentSessionMessageSendMutation({
      sessionId,
      // Orca's ledger wants `<13-digit ms timestamp>-<32 hex>` (AGENT_SESSION_OPERATION_ID_PATTERN).
      clientOperationId: `${Date.now()}-${randomBytes(16).toString("hex")}`,
      expectedRuntimeFence: page.fence ?? null,
      body: send.structuredAgentSessionSendBody(text, []),
    });
    const answer = (await client.call("agentSession.send", mutation)).result;
    if (!answer || answer.ok !== true) {
      throw new Error(answer?.refusal?.details?.reason || answer?.refusal?.code || "send refused");
    }
    process.stdout.write(JSON.stringify({ sent: true, dispatch: answer.value?.submission?.dispatchState ?? null }) + "\n");
    return;
  }
  throw new Error(`unknown mode "${mode}": use history, say or answer`);
})().catch((error) => {
  process.stdout.write(JSON.stringify({ error: String(error?.message ?? error) }) + "\n");
  process.exit(1);
});
