// vitals_reader.swift - live passive reader for the Baby Sensor Relax wristband on macOS.
//
// Receive-only: it scans BLE advertisements and never connects, writes or pairs, so it cannot
// affect the wristband's link to its base. It prints one line per new measurement.
//
// Frame layout (23 bytes of manufacturer data; the full account is in docs/PROTOCOL.md):
//   b0     0xF5 marker
//   b1     0x03 device type (wristband)
//   b2     measurement counter, +1 per new reading (~20 s)
//   b3     state bits: 0x80 idle/charging, 0x08 new temperature committed, 0x04 new SpO2 committed
//   b4     signal-quality hint
//   b6-7   skin temperature, big-endian uint16 / 10 (0.1 C, updates ~every 15 min)
//   b8-9   second thermal value, uint16 / 10, meaning not confirmed
//   b10    heart rate (bpm)
//   b11-12 inter-beat interval, big-endian, milliseconds
//   b13    SpO2 (%)
//   b14-15 optical return level, not a vital sign
//   b16-21 MAC address, little-endian;  b22 constant
//
// Build: swiftc -O mac/vitals_reader.swift -o vitals_reader
// Usage: ./vitals_reader [seconds]   (default 3600)

import Foundation
import CoreBluetooth
setbuf(stdout, nil)

let RUN = Double(CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1]) ?? 3600 : 3600)
let fmt: DateFormatter = { let f = DateFormatter(); f.dateFormat = "HH:mm:ss"; return f }()

final class Reader: NSObject, CBCentralManagerDelegate {
    var cm: CBCentralManager!
    var lastSeq = -1
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            print("Baby Sensor Relax - passive reader (receive-only). Ctrl-C to stop.")
            print("time     | HR  beat | SpO2 | skin   | signal  | RSSI")
            print("---------+----------+------+--------+---------+-----")
            c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        case .unauthorized:
            print("Not authorised: grant Bluetooth permission to the terminal (System Settings > Privacy & Security > Bluetooth).")
        default:
            print("Bluetooth state=\(c.state.rawValue); Bluetooth must be on.")
        }
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData a: [String: Any], rssi r: NSNumber) {
        let nm = (a[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        guard nm.uppercased().contains("BS01"),
              let m = a[CBAdvertisementDataManufacturerDataKey] as? Data,
              m.count >= 23, m[0] == 0xF5, m[1] == 0x03 else { return }
        let b = [UInt8](m)
        let seq = Int(b[2])
        guard seq != lastSeq else { return }   // one line per new reading
        lastSeq = seq

        let hr = Int(b[10])
        let beatMs = Int(b[11]) << 8 | Int(b[12])
        let spo2 = Int(b[13])
        let sig = Int(b[4])
        let skin = Double((UInt16(b[6]) << 8) | UInt16(b[7])) / 10.0
        let plausible = skin >= 28.0 && skin <= 42.0   // the same gate as band_protocol.h
        let skinStr = plausible ? String(format: "%.1fC", skin) : "--"
        let t = fmt.string(from: Date())
        print(String(format: "%@ | %3d %4dms | %3d%% | %-6@ | sig=%-3d | %d",
                     t, hr, beatMs, spo2, skinStr as NSString, sig, r.intValue))
    }
}

let rd = Reader()
rd.cm = CBCentralManager(delegate: rd, queue: nil)
DispatchQueue.main.asyncAfter(deadline: .now() + RUN) { rd.cm.stopScan(); exit(0) }
RunLoop.main.run()
