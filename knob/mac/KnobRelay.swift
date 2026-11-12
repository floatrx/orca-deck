// KnobRelay.app — carries the knob's relay lines over BLE. macOS grants Bluetooth to an app bundle, not to a
// python run by uv (Phase 1 spike, .work/plans/knob-ble.md), so this app owns CoreBluetooth and runs
// `relay.py --stdio` as a child: BLE notifications → the child's stdin, the child's stdout → BLE writes.
// The bridge logic and the S / K / D / A / C protocol stay in relay.py.
//
// Security: the app talks only to the knob it pinned on the first pairing (~/.config/orca-deck/knob.json,
// a CoreBluetooth peripheral identifier). `--pair` forgets the pin and takes the next orca-deck-knob it sees.
//
// Usage: KnobRelay <path/to/relay.py> [--pair]
import CoreBluetooth
import Foundation

let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let rxUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")  // the knob receives (we write)
let txUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")  // the knob sends (we subscribe)
let knobName = "orca-deck-knob"
let pinFile = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".config/orca-deck/knob.json")

func log(_ text: String) {
    let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
    FileHandle.standardError.write("\(stamp) app: \(text)\n".data(using: .utf8)!)
}

func loadPin() -> UUID? {
    guard let data = try? Data(contentsOf: pinFile),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: String],
          let id = json["peripheral"] else { return nil }
    return UUID(uuidString: id)
}

func savePin(_ id: UUID) {
    let json = ["peripheral": id.uuidString]
    try? FileManager.default.createDirectory(at: pinFile.deletingLastPathComponent(), withIntermediateDirectories: true)
    if let data = try? JSONSerialization.data(withJSONObject: json) {
        FileManager.default.createFile(atPath: pinFile.path, contents: data, attributes: [.posixPermissions: 0o600])
    }
}

/// relay.py as a child process, restarted when it exits.
final class Relay {
    let script: String
    var process: Process?
    var stdin: FileHandle?
    var onLine: ((Data) -> Void)?
    private var buffer = Data()

    init(script: String) { self.script = script }

    func start() {
        let child = Process()
        child.executableURL = URL(fileURLWithPath: "/opt/homebrew/bin/uv")
        child.arguments = ["run", script, "--stdio"]
        let input = Pipe(), output = Pipe()
        child.standardInput = input
        child.standardOutput = output
        child.standardError = FileHandle.standardError
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let chunk = handle.availableData
            guard !chunk.isEmpty else { return }
            DispatchQueue.main.async { self?.take(chunk) }
        }
        child.terminationHandler = { [weak self] proc in
            log("relay.py exited (\(proc.terminationStatus)), restarting in 2 s")
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self?.start() }
        }
        do {
            try child.run()
            process = child
            stdin = input.fileHandleForWriting
            log("relay.py started (pid \(child.processIdentifier))")
        } catch {
            log("cannot start relay.py: \(error)")
        }
    }

    private func take(_ chunk: Data) {
        buffer.append(chunk)
        while let end = buffer.firstIndex(of: 0x0A) {
            let line = buffer[buffer.startIndex...end]  // with the newline
            buffer.removeSubrange(buffer.startIndex...end)
            onLine?(Data(line))
        }
    }

    func write(_ line: Data) {
        try? stdin?.write(contentsOf: line)
    }
}

final class Link: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    let relay: Relay
    var central: CBCentralManager!
    var knob: CBPeripheral?
    var rx: CBCharacteristic?
    var pinned: UUID?
    var outbox = Data()      // lines for the knob, sent in write-without-response chunks
    var inbox = Data()       // notification bytes until a newline

    init(relay: Relay, pair: Bool) {
        self.relay = relay
        super.init()
        pinned = pair ? nil : loadPin()
        relay.onLine = { [weak self] line in self?.send(line) }
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var ready: Bool { knob?.state == .connected && rx != nil }

    func send(_ line: Data) {
        // While the knob is away, only the newest state matters: drop what piles up.
        guard ready else { return }
        outbox.append(line)
        pump()
    }

    func pump() {
        guard let knob, let rx else { return }
        let size = knob.maximumWriteValueLength(for: .withoutResponse)
        while !outbox.isEmpty && knob.canSendWriteWithoutResponse {
            let n = min(size, outbox.count)
            knob.writeValue(outbox.prefix(n), for: rx, type: .withoutResponse)
            outbox.removeFirst(n)
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        log("bluetooth state \(central.state.rawValue), authorization \(CBManager.authorization.rawValue)")
        guard central.state == .poweredOn else { return }
        if let pinned, let known = central.retrievePeripherals(withIdentifiers: [pinned]).first {
            connect(known)  // pending until the knob is in range: CoreBluetooth connects have no timeout
        } else {
            log(pinned == nil ? "pairing: looking for \(knobName)" : "pinned knob unknown to this Mac, scanning")
            central.scanForPeripherals(withServices: [serviceUUID])
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? ""
        if let pinned, peripheral.identifier != pinned { return }
        guard name == knobName else { return }
        central.stopScan()
        log("found \(name) \(peripheral.identifier) rssi \(rssi)")
        connect(peripheral)
    }

    func connect(_ peripheral: CBPeripheral) {
        knob = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("connected")
        if pinned == nil {
            pinned = peripheral.identifier
            savePin(peripheral.identifier)
            log("pinned \(peripheral.identifier)")
        }
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("disconnected \(error?.localizedDescription ?? "")")
        rx = nil
        outbox.removeAll()
        inbox.removeAll()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.connect(peripheral) }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        log("connect failed \(error?.localizedDescription ?? "")")
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.connect(peripheral) }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else { return }
        peripheral.discoverCharacteristics([rxUUID, txUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        // TX needs an encrypted link to be read (READ_ENC): reading it first makes macOS pair (Just Works) or
        // re-encrypt with the bond, then the read completes and we subscribe. Subscribing alone needs no
        // encryption, and a security request from the knob is ignored by macOS.
        if let tx = service.characteristics?.first(where: { $0.uuid == txUUID }) { peripheral.readValue(for: tx) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            log("subscribe failed: \(error.localizedDescription)")
            return
        }
        rx = characteristic.service?.characteristics?.first(where: { $0.uuid == rxUUID })
        log("ready (mtu \(peripheral.maximumWriteValueLength(for: .withoutResponse) + 3))")
        relay.write("C {\"cmd\":\"hello\"}\n".data(using: .utf8)!)  // the relay resends the full state now
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) { pump() }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == txUUID else { return }
        if !characteristic.isNotifying {
            // The encrypted read finished (or failed): the link is secure now, subscribe.
            if let error { log("encrypted read failed: \(error.localizedDescription)"); return }
            log("encrypted")
            peripheral.setNotifyValue(true, for: characteristic)
            return
        }
        guard let value = characteristic.value else { return }
        inbox.append(value)
        while let end = inbox.firstIndex(of: 0x0A) {
            let line = inbox[inbox.startIndex...end]
            inbox.removeSubrange(inbox.startIndex...end)
            relay.write(Data(line))
        }
    }
}

let args = Array(CommandLine.arguments.dropFirst())
guard let script = args.first(where: { !$0.hasPrefix("--") }) else {
    FileHandle.standardError.write("usage: KnobRelay <relay.py> [--pair]\n".data(using: .utf8)!)
    exit(2)
}
let relay = Relay(script: script)
let link = Link(relay: relay, pair: args.contains("--pair"))
relay.start()
signal(SIGTERM) { _ in
    relay.process?.terminate()
    exit(0)
}
RunLoop.main.run()
