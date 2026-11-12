// Bluetooth (TCC) is granted to an app with NSBluetoothAlwaysUsageDescription, and a child process inherits it.
// This launcher is that app: it runs `uv run <script> <args…>` as a child and mirrors its exit code.
import Foundation

let args = Array(CommandLine.arguments.dropFirst())
guard let script = args.first else {
    FileHandle.standardError.write("usage: launcher <script.py> [args…]\n".data(using: .utf8)!)
    exit(2)
}
let child = Process()
child.executableURL = URL(fileURLWithPath: "/opt/homebrew/bin/uv")
child.arguments = ["run", script] + args.dropFirst()
do {
    try child.run()
} catch {
    FileHandle.standardError.write("launcher: \(error)\n".data(using: .utf8)!)
    exit(1)
}
signal(SIGTERM) { _ in exit(0) }
child.waitUntilExit()
exit(child.terminationStatus)
