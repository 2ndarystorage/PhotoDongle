import Foundation
import CoreBluetooth

/// Talks to the PHOTO-DONGLE firmware (custom GATT service 0xFFF0).
///
/// Protocol:
///  - CTRL (FFF1, write): [0x01][u32 LE size]=BEGIN, [0x02]=COMMIT,
///    [0x03][u16 LE sec][u8 order][u8 scale]=SETTINGS, [0x04]=CLEAR, [0x05]=NEXT,
///    [0x06]=VJ_BEGIN, [0x07]=VJ_PLAY, [0x08]=VJ_STOP, [0x09]=VJ_RESUME
///  - DATA (FFF2, write): raw frame bytes while receiving
///  - STAT (FFF3, read/notify): [u8 state][u16 LE count][u16 LE current]
///                              [u16 LE interval][u8 order][u8 scale]
///    state: 0=idle 1=receiving 2=error 4=VJ playing
final class BLEManager: NSObject, ObservableObject {
    static let serviceUUID = CBUUID(string: "FFF0")
    static let ctrlUUID = CBUUID(string: "FFF1")
    static let dataUUID = CBUUID(string: "FFF2")
    static let statUUID = CBUUID(string: "FFF3")

    @Published var connectionState: String = "未接続"
    @Published var isConnected = false
    @Published var devicePhotoCount: Int = 0
    @Published var deviceCurrent: Int = 0
    @Published var deviceState: Int = 0
    @Published var sendProgress: Double? = nil   // 0...1 while sending one frame
    @Published var queueLabel: String? = nil     // e.g. "3 / 24" during a queue
    @Published var log: String = ""

    var isSending: Bool { sendProgress != nil }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var ctrlChar: CBCharacteristic?
    private var dataChar: CBCharacteristic?

    // transfer state
    private var pendingFrame: Data?
    private var sendOffset = 0
    private var sendChunk = 180
    private var sendQueue: [Data] = []
    private var queueTotal = 0
    private var queueDone = 0
    private var queueCompletion: (() -> Void)?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    private func note(_ s: String) {
        log = s
        print("[BLE] \(s)")
    }

    // MARK: - Connection

    func toggleConnection() {
        if isConnected || peripheral != nil {
            if let p = peripheral { central.cancelPeripheralConnection(p) }
            peripheral = nil
            return
        }
        guard central.state == .poweredOn else {
            note("Bluetoothが使えません（状態: \(central.state.rawValue)）")
            return
        }
        connectionState = "検索中…"
        central.scanForPeripherals(withServices: [Self.serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        // Fallback: also accept the advertised name if the service UUID is not in the adv packet.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            guard let self, self.peripheral == nil else { return }
            self.central.stopScan()
            self.central.scanForPeripherals(withServices: nil, options: nil)
        }
    }

    // MARK: - Commands

    private func writeCtrl(_ data: Data) {
        guard let p = peripheral, let c = ctrlChar else { return }
        p.writeValue(data, for: c, type: .withResponse)
    }

    func sendSettings(interval: Int, order: DisplayOrder, scale: ScaleMode) {
        var d = Data([0x03])
        d.append(UInt8(interval & 0xFF))
        d.append(UInt8((interval >> 8) & 0xFF))
        d.append(UInt8(order.rawValue))
        d.append(UInt8(scale.rawValue))
        writeCtrl(d)
        note("設定を送信: \(interval)秒 / \(order.label) / \(scale.label)")
    }

    func sendNext() { writeCtrl(Data([0x05])) }
    func clearAll() { writeCtrl(Data([0x04])); note("全削除を送信") }

    // VJ mode
    func vjBegin() { writeCtrl(Data([0x06])) }
    func vjPlay() { writeCtrl(Data([0x07])); note("VJ再生を送信") }
    func vjStop() { writeCtrl(Data([0x08])); note("VJ停止を送信") }
    func vjResume() { writeCtrl(Data([0x09])); note("VJ再開を送信") }

    // MARK: - Frame transfer (single + queue)

    func sendImage(frame: Data) {
        sendImages(frames: [frame])
    }

    func sendImages(frames: [Data], completion: (() -> Void)? = nil) {
        guard isConnected, peripheral != nil, dataChar != nil else {
            note("未接続のため送信できません")
            return
        }
        guard sendProgress == nil else {
            note("送信中です")
            return
        }
        let valid = frames.filter { $0.count == ImageConverter.frameBytes }
        guard !valid.isEmpty else {
            note("フレームサイズ異常のため送信できません")
            return
        }
        sendQueue = valid
        queueTotal = valid.count
        queueDone = 0
        queueCompletion = completion
        updateQueueLabel()
        startNextInQueue()
    }

    /// Send a whole VJ frame set: VJ_BEGIN, the frames, then VJ_PLAY.
    func sendVJFrames(frames: [Data]) {
        guard isConnected else {
            note("未接続のため送信できません")
            return
        }
        vjBegin()
        note("VJフレーム \(frames.count) 枚を送信します")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.sendImages(frames: frames) { [weak self] in
                self?.note("VJフレーム送信完了、再生開始")
                self?.vjPlay()
            }
        }
    }

    private func updateQueueLabel() {
        queueLabel = queueTotal > 1 ? "\(queueDone + 1) / \(queueTotal)" : nil
    }

    private func startNextInQueue() {
        guard let p = peripheral, !sendQueue.isEmpty else {
            finishQueue()
            return
        }
        let frame = sendQueue.removeFirst()
        sendChunk = max(20, p.maximumWriteValueLength(for: .withResponse))
        pendingFrame = frame
        sendOffset = 0
        sendProgress = 0
        var begin = Data([0x01])
        begin.append(UInt8(frame.count & 0xFF))
        begin.append(UInt8((frame.count >> 8) & 0xFF))
        begin.append(UInt8((frame.count >> 16) & 0xFF))
        begin.append(UInt8((frame.count >> 24) & 0xFF))
        writeCtrl(begin)
        note("送信開始（\(queueDone + 1)/\(queueTotal) 枚目, chunk \(sendChunk)）")
        // Give the firmware a moment to open the temp file, then stream.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            self?.sendNextChunk()
        }
    }

    private func finishQueue() {
        sendProgress = nil
        queueLabel = nil
        let done = queueCompletion
        queueCompletion = nil
        if queueTotal > 1 { note("\(queueTotal) 枚の送信が完了しました") }
        done?()
    }

    private func sendNextChunk() {
        guard let frame = pendingFrame, let p = peripheral, let dc = dataChar else { return }
        if sendOffset >= frame.count {
            writeCtrl(Data([0x02]))  // COMMIT
            note("送信完了、保存中…")
            pendingFrame = nil
            queueDone += 1
            if sendQueue.isEmpty {
                finishQueue()
            } else {
                updateQueueLabel()
                // Let the firmware rename + rescan before the next BEGIN.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    self?.startNextInQueue()
                }
            }
            return
        }
        let end = min(frame.count, sendOffset + sendChunk)
        p.writeValue(frame.subdata(in: sendOffset..<end), for: dc, type: .withResponse)
        sendOffset = end
        sendProgress = Double(sendOffset) / Double(frame.count)
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state != .poweredOn {
            connectionState = "Bluetooth不可"
            isConnected = false
        } else if peripheral == nil {
            connectionState = "未接続"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? ""
        let advServices = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        guard name == "PHOTO-DONGLE" || advServices.contains(Self.serviceUUID) else { return }
        self.peripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        connectionState = "接続中…"
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectionState = "サービス検索中…"
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        connectionState = "未接続"
        ctrlChar = nil; dataChar = nil
        self.peripheral = nil
        sendProgress = nil
        queueLabel = nil
        pendingFrame = nil
        sendQueue = []
        queueCompletion = nil
        note("切断されました")
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        connectionState = "接続失敗"
        self.peripheral = nil
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            note("サービスFFF0が見つかりません")
            return
        }
        peripheral.discoverCharacteristics([Self.ctrlUUID, Self.dataUUID, Self.statUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] {
            switch c.uuid {
            case Self.ctrlUUID: ctrlChar = c
            case Self.dataUUID: dataChar = c
            case Self.statUUID:
                peripheral.setNotifyValue(true, for: c)
                peripheral.readValue(for: c)
            default: break
            }
        }
        if ctrlChar != nil && dataChar != nil {
            isConnected = true
            connectionState = "接続済み"
            note("PHOTO-DONGLEに接続しました")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            note("書き込みエラー: \(error.localizedDescription)")
            sendProgress = nil
            queueLabel = nil
            pendingFrame = nil
            sendQueue = []
            queueCompletion = nil
            return
        }
        if characteristic.uuid == Self.dataUUID {
            sendNextChunk()
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.statUUID, let v = characteristic.value, v.count >= 9 else { return }
        deviceState = Int(v[0])
        devicePhotoCount = Int(v[1]) | (Int(v[2]) << 8)
        deviceCurrent = Int(v[3]) | (Int(v[4]) << 8)
        if deviceState == 2 {
            note("デバイス側でエラーが発生しました")
        }
    }
}
