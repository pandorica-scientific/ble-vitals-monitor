// vitals_reader.swift — live passive reader for the Baby Sensor Relax wristband.
//
// RECEIVE-ONLY: scans BLE advertisements only. Never connects, writes, or pairs, so it
// cannot affect the band<->base link. It just decodes what the band already broadcasts.
//
// Decoded from the 23-byte manufacturer-data frame (device type 0x03 = wristband):
//   b0    0xF5 marker
//   b1    0x03 device type (wristband)
//   b2    measurement counter (+1 per new reading, ~20s)
//   b3    state; bit 0x08 set on the packet that introduces a new temperature (candidate flag)
//   b4    signal-quality hint
//   b6-7  SKIN TEMPERATURE: big-endian uint16 / 10  (0.1 C; updates ~every 15 min)
//   b8-9  second uint16/10 thermal value (meaning not confirmed)
//   b10   heart rate (bpm)
//   b11:12, b14:15  PPG / perfusion signal (16-bit)
//   b13   SpO2 (%)
//   b16-21 MAC (little-endian)  b22 checksum/const
//
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
            print("Baby Sensor Relax — passive reader (RECEIVE-ONLY). Ctrl-C to stop.")
            print("time     | HR   SpO2 | Skin    Body   | signal  | RSSI")
            print("---------+-----------+----------------+---------+-----")
            c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        case .unauthorized:
            print("NOT AUTHORIZED — grant Bluetooth permission to the terminal (System Settings > Privacy > Bluetooth).")
        default:
            print("Bluetooth state=\(c.state.rawValue) — need Bluetooth ON.")
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
        let spo2 = Int(b[13])
        let sig = Int(b[4])                 // signal/motion hint (raw; exact meaning unpinned)
        // Skin temp = big-endian uint16 of bytes 6-7, /10 (0.1 C resolution). Confirmed.
        let skin = Double((UInt16(b[6]) << 8) | UInt16(b[7])) / 10.0
        let plausible = skin >= 28.0 && skin <= 42.0
        let skinStr = plausible ? String(format: "%.1fC", skin) : "  --  "
        let t = fmt.string(from: Date())
        print(String(format: "%@ | %3d  %3d%% | %-6@ | sig=%-3d | %d",
                     t, hr, spo2, skinStr as NSString, sig, r.intValue))
    }
}

let rd = Reader()
rd.cm = CBCentralManager(delegate: rd, queue: nil)
DispatchQueue.main.asyncAfter(deadline: .now() + RUN) { rd.cm.stopScan(); exit(0) }
RunLoop.main.run()
