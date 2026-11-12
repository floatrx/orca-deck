// Spike: does this app itself get Bluetooth? Prints the CoreBluetooth state and the authorization.
import CoreBluetooth
import Foundation

final class Probe: NSObject, CBCentralManagerDelegate {
    var manager: CBCentralManager!
    override init() {
        super.init()
        manager = CBCentralManager(delegate: self, queue: nil)
    }
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let names = ["unknown", "resetting", "unsupported", "unauthorized", "poweredOff", "poweredOn"]
        print("state=\(names[central.state.rawValue]) authorization=\(CBManager.authorization.rawValue)")
        fflush(stdout)
        exit(0)
    }
}
let probe = Probe()
RunLoop.main.run(until: Date().addingTimeInterval(20))
print("state=timeout authorization=\(CBManager.authorization.rawValue)")
