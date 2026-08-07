// passive_scan.swift — RECEIVE-ONLY BLE observer for the Baby Sensor Relax system.
//
// HARD SAFETY RULE (carried over from owlet-gatt-probe):
//   - SCAN ONLY. Never connect(), never write, never pair, never subscribe.
//   - This program cannot break the base<->wristband link because it never transmits
//     anything to either device; it only listens to advertisements the wristband/base
//     already broadcast to the whole room.
//
// Goal: find the wristband (BS01 / "Oliwia") and base (BG01N) advertisements and see
// whether their payload bytes change over time — the signature of live vitals.

import Foundation
import CoreBluetooth

let RUN_SECONDS: Double = Double(CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1]) ?? 60 : 60)

// Name fragments that likely identify our devices. macOS hides the BLE MAC (gives a
// synthetic UUID instead), so we match on advertised local name + payload content.
let NAME_HINTS = ["BS01", "BG01", "BABYSENSOR", "BABY SENSOR", "RELAX", "OLIWIA", "M0L2"]

func hex(_ d: Data) -> String { d.map { String(format: "%02X", $0) }.joined() }

func hammingHexDiff(_ a: Data, _ b: Data) -> Int {
    let n = min(a.count, b.count)
    var diff = abs(a.count - b.count) * 8
    for i in 0..<n { diff += (a[i] ^ b[i]).nonzeroBitCount }
    return diff
}

final class Seen {
    var name: String?
    var firstRSSI: Int = 0
    var lastRSSI: Int = 0
    var count = 0
    var serviceUUIDs = Set<String>()
    var lastMfg: Data?
    var mfgChanges = 0
    var mfgSamples = [Data]()          // keep a few to show variability
    var lastSvcData: [String: Data] = [:]
    var svcDataChanges = 0
    var interesting = false
}

final class Observer: NSObject, CBCentralManagerDelegate {
    var central: CBCentralManager!
    var devices: [UUID: Seen] = [:]
    let start = Date()

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            FileHandle.standardError.write("[scan] Bluetooth ON — passive scan for \(Int(RUN_SECONDS))s (RECEIVE-ONLY)\n".data(using: .utf8)!)
            // allowDuplicates:true => we get every advertisement, so we can watch payloads change.
            c.scanForPeripherals(withServices: nil,
                                 options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        case .poweredOff:
            FileHandle.standardError.write("[scan] Bluetooth is OFF — enable it and retry.\n".data(using: .utf8)!)
        case .unauthorized:
            FileHandle.standardError.write("[scan] NOT AUTHORIZED — grant Bluetooth permission to the terminal in System Settings > Privacy > Bluetooth.\n".data(using: .utf8)!)
        default:
            FileHandle.standardError.write("[scan] state=\(c.state.rawValue)\n".data(using: .utf8)!)
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData adv: [String: Any], rssi RSSI: NSNumber) {
        let id = p.identifier
        let s = devices[id] ?? Seen()
        s.count += 1
        s.lastRSSI = RSSI.intValue
        if s.count == 1 { s.firstRSSI = RSSI.intValue }

        let advName = (adv[CBAdvertisementDataLocalNameKey] as? String) ?? p.name
        if let nm = advName { s.name = nm }

        if let uuids = adv[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] {
            uuids.forEach { s.serviceUUIDs.insert($0.uuidString) }
        }

        // Manufacturer-specific data — most likely place for broadcast vitals.
        if let mfg = adv[CBAdvertisementDataManufacturerDataKey] as? Data {
            if let last = s.lastMfg, last != mfg { s.mfgChanges += 1 }
            s.lastMfg = mfg
            if s.mfgSamples.count < 6 && !s.mfgSamples.contains(mfg) { s.mfgSamples.append(mfg) }
        }
        // Service data — the other common place.
        if let sd = adv[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {
            for (k, v) in sd {
                let key = k.uuidString
                if let last = s.lastSvcData[key], last != v { s.svcDataChanges += 1 }
                s.lastSvcData[key] = v
            }
        }

        // Flag if the name matches our device hints.
        if let nm = s.name?.uppercased(), NAME_HINTS.contains(where: { nm.contains($0) }) {
            if !s.interesting {
                s.interesting = true
                FileHandle.standardError.write("[hit] name-match '\(s.name ?? "?")' id=\(id.uuidString.prefix(8)) rssi=\(s.lastRSSI)\n".data(using: .utf8)!)
            }
        }
        devices[id] = s
    }

    func report() {
        print("\n================ PASSIVE SCAN SUMMARY ================")
        print(String(format: "duration: %.0fs   devices seen: %d\n", Date().timeIntervalSince(start), devices.count))

        // Interesting (name-matched) first, then anything with changing payloads, then the rest.
        let sorted = devices.sorted { a, b in
            if a.value.interesting != b.value.interesting { return a.value.interesting }
            let ca = a.value.mfgChanges + a.value.svcDataChanges
            let cb = b.value.mfgChanges + b.value.svcDataChanges
            if ca != cb { return ca > cb }
            return a.value.count > b.value.count
        }
        for (id, s) in sorted {
            let changing = s.mfgChanges + s.svcDataChanges
            let tag = s.interesting ? "  <== NAME MATCH" : (changing > 3 ? "  <== payload changes over time" : "")
            print("• id=\(id.uuidString.prefix(8))  name=\(s.name ?? "(none)")  adv#=\(s.count)  rssi=\(s.firstRSSI)->\(s.lastRSSI)\(tag)")
            if !s.serviceUUIDs.isEmpty { print("    services: \(s.serviceUUIDs.sorted().joined(separator: ", "))") }
            if let m = s.lastMfg { print("    mfgData(\(m.count)B) changes=\(s.mfgChanges): \(hex(m))") }
            if s.mfgSamples.count > 1 {
                for (i, smp) in s.mfgSamples.enumerated() {
                    let d = i > 0 ? hammingHexDiff(s.mfgSamples[0], smp) : 0
                    print("      mfg[\(i)] \(hex(smp))  (bitdiff vs [0]=\(d))")
                }
            }
            for (k, v) in s.lastSvcData {
                print("    svcData[\(k)](\(v.count)B) changes=\(s.svcDataChanges): \(hex(v))")
            }
        }
        print("=====================================================")
    }
}

let obs = Observer()
obs.central = CBCentralManager(delegate: obs, queue: nil)

DispatchQueue.main.asyncAfter(deadline: .now() + RUN_SECONDS) {
    obs.central.stopScan()
    obs.report()
    exit(0)
}
RunLoop.main.run()
