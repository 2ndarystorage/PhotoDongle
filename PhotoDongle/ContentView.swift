import SwiftUI
import PhotosUI

struct ContentView: View {
    @EnvironmentObject var ble: BLEManager

    @State private var pickerItems: [PhotosPickerItem] = []
    @State private var sourceImages: [UIImage] = []
    @State private var previewImage: UIImage?
    @State private var frames: [Data] = []

    @AppStorage("intervalSec") private var intervalSec: Int = 5
    @AppStorage("orderRaw") private var orderRaw: Int = 0
    // Default scale is 拡大 (cover). The fallback raw value is 1, and a
    // one-time migration below moves existing installs to cover as well.
    @AppStorage("scaleRaw") private var scaleRaw: Int = 1
    @AppStorage("anchorXRaw") private var anchorXRaw: Int = 1  // 0 left, 1 center, 2 right
    @AppStorage("anchorYRaw") private var anchorYRaw: Int = 1  // 0 top, 1 center, 2 bottom
    @AppStorage("didSetCoverDefault") private var didSetCoverDefault: Bool = false

    @State private var showClearConfirm = false

    // VJ
    @AppStorage("vjEffectRaw") private var vjEffectRaw: Int = 1
    @AppStorage("vjFrameCount") private var vjFrameCount: Int = 24
    @State private var vjBusy = false
    @State private var vjStatus: String = ""

    private var order: DisplayOrder { DisplayOrder(rawValue: orderRaw) ?? .sequential }
    private var scale: ScaleMode { ScaleMode(rawValue: scaleRaw) ?? .cover }
    private var anchorX: CGFloat { CGFloat(anchorXRaw) / 2 }
    private var anchorY: CGFloat { CGFloat(anchorYRaw) / 2 }
    private var vjEffect: VJEffect { VJEffect(rawValue: vjEffectRaw) ?? .psychedelic }

    var body: some View {
        NavigationStack {
            Form {
                Section("接続") {
                    HStack {
                        Circle()
                            .fill(ble.isConnected ? Color.green : Color.gray)
                            .frame(width: 10, height: 10)
                        Text(ble.connectionState)
                        Spacer()
                        Button(ble.isConnected ? "切断" : "接続") { ble.toggleConnection() }
                            .buttonStyle(.bordered)
                    }
                    if ble.isConnected {
                        LabeledContent("保存済み写真", value: "\(ble.devicePhotoCount) 枚")
                        LabeledContent("表示中", value: ble.devicePhotoCount > 0 ? "No.\(ble.deviceCurrent)" : "-")
                        if ble.deviceState == 4 {
                            LabeledContent("状態", value: "VJ再生中")
                        }
                    }
                    if !ble.log.isEmpty {
                        Text(ble.log).font(.caption).foregroundStyle(.secondary)
                    }
                }

                Section("写真を送る（複数選択OK）") {
                    PhotosPicker(selection: $pickerItems, maxSelectionCount: 20,
                                 matching: .images, photoLibrary: .shared()) {
                        Label("写真アプリから選ぶ", systemImage: "photo.on.rectangle.angled")
                    }
                    if !sourceImages.isEmpty {
                        Text("\(sourceImages.count) 枚選択中")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if let previewImage {
                        HStack {
                            Spacer()
                            Image(uiImage: previewImage)
                                .interpolation(.none)
                                .resizable()
                                .frame(width: 240, height: 120)
                                .border(Color.gray)
                            Spacer()
                        }
                        Text("送信プレビュー（1枚目 / 160×80 / \(scale.label)）")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if ble.isSending {
                        ProgressView(value: ble.sendProgress ?? 0) {
                            if let q = ble.queueLabel {
                                Text("送信中… \(q) 枚目 \(Int((ble.sendProgress ?? 0) * 100))%")
                            } else {
                                Text("送信中… \(Int((ble.sendProgress ?? 0) * 100))%")
                            }
                        }
                    } else {
                        Button {
                            ble.sendImages(frames: frames)
                        } label: {
                            Label(frames.count > 1 ? "まとめて送信（\(frames.count)枚）" : "ドングルへ送信",
                                  systemImage: "paperplane")
                        }
                        .disabled(frames.isEmpty || !ble.isConnected)
                    }
                }

                Section("表示設定（ドングル）") {
                    Stepper(value: $intervalSec, in: 1...60) {
                        LabeledContent("表示間隔", value: "\(intervalSec) 秒")
                    }
                    Picker("表示順", selection: $orderRaw) {
                        ForEach(DisplayOrder.allCases) { Text($0.label).tag($0.rawValue) }
                    }
                    Picker("サイズ調整", selection: $scaleRaw) {
                        ForEach(ScaleMode.allCases) { Text($0.label).tag($0.rawValue) }
                    }
                    if scale == .cover {
                        Picker("横の位置", selection: $anchorXRaw) {
                            Text("左").tag(0)
                            Text("中央").tag(1)
                            Text("右").tag(2)
                        }
                        Picker("縦の位置", selection: $anchorYRaw) {
                            Text("上").tag(0)
                            Text("中央").tag(1)
                            Text("下").tag(2)
                        }
                        Text("「拡大」で切り抜くときに、写真のどの部分を表示するか選べます。")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Button("設定をドングルへ送信") {
                        ble.sendSettings(interval: intervalSec, order: order, scale: scale)
                    }
                    .disabled(!ble.isConnected)
                    Text("サイズ調整と位置は、写真を変換するときに適用されます。")
                        .font(.caption).foregroundStyle(.secondary)
                }

                Section("VJモード") {
                    Picker("効果", selection: $vjEffectRaw) {
                        ForEach(VJEffect.allCases) { Text($0.label).tag($0.rawValue) }
                    }
                    Picker("フレーム数", selection: $vjFrameCount) {
                        Text("16").tag(16)
                        Text("24").tag(24)
                        Text("32").tag(32)
                    }
                    if vjBusy {
                        ProgressView { Text("フレーム生成中…") }
                    } else {
                        Button {
                            Task { await generateAndSendVJ() }
                        } label: {
                            Label("VJフレームを作って送信", systemImage: "sparkles")
                        }
                        .disabled(sourceImages.isEmpty || !ble.isConnected || ble.isSending)
                    }
                    if !vjStatus.isEmpty {
                        Text(vjStatus).font(.caption).foregroundStyle(.secondary)
                    }
                    HStack {
                        Button("VJ再生") { ble.vjPlay() }
                            .disabled(!ble.isConnected)
                        Spacer()
                        Button("VJ停止") { ble.vjStop() }
                            .disabled(!ble.isConnected)
                    }
                    Text("選択中の1枚目からアニメーションを作り、ドングルでループ再生します。送信には1〜2分かかります。")
                        .font(.caption).foregroundStyle(.secondary)
                }

                Section("操作") {
                    Button {
                        ble.sendNext()
                    } label: {
                        Label("次の写真へ（VJ中は終了）", systemImage: "forward.frame")
                    }
                    .disabled(!ble.isConnected)
                    Button(role: .destructive) {
                        showClearConfirm = true
                    } label: {
                        Label("写真を全て削除", systemImage: "trash")
                    }
                    .disabled(!ble.isConnected)
                }
            }
            .navigationTitle("Photo Dongle")
            .confirmationDialog("SDカード内の写真を全て削除しますか？",
                                isPresented: $showClearConfirm, titleVisibility: .visible) {
                Button("削除する", role: .destructive) { ble.clearAll() }
                Button("キャンセル", role: .cancel) {}
            }
            .onAppear {
                if !didSetCoverDefault {
                    scaleRaw = ScaleMode.cover.rawValue
                    didSetCoverDefault = true
                }
            }
            .onChange(of: pickerItems) { _, newItems in
                Task { await loadPicked(newItems) }
            }
            .onChange(of: scaleRaw) { _, _ in reconvert() }
            .onChange(of: anchorXRaw) { _, _ in reconvert() }
            .onChange(of: anchorYRaw) { _, _ in reconvert() }
        }
    }

    private func loadPicked(_ items: [PhotosPickerItem]) async {
        var images: [UIImage] = []
        for item in items {
            if let data = try? await item.loadTransferable(type: Data.self),
               let ui = UIImage(data: data) {
                images.append(ui)
            }
        }
        sourceImages = images
        reconvert()
    }

    private func reconvert() {
        frames = sourceImages.compactMap {
            ImageConverter.convert($0, mode: scale, anchorX: anchorX, anchorY: anchorY)?.frame
        }
        if let first = sourceImages.first,
           let out = ImageConverter.convert(first, mode: scale, anchorX: anchorX, anchorY: anchorY) {
            previewImage = out.preview
        } else {
            previewImage = nil
        }
    }

    private func generateAndSendVJ() async {
        guard let img = sourceImages.first else { return }
        vjBusy = true
        vjStatus = "フレームを生成しています…"
        let effect = vjEffect
        let count = vjFrameCount
        let generated = await Task.detached(priority: .userInitiated) {
            ImageConverter.vjFrames(from: img, effect: effect, count: count)
        }.value
        vjBusy = false
        guard !generated.isEmpty else {
            vjStatus = "フレーム生成に失敗しました"
            return
        }
        vjStatus = "\(generated.count) フレームを生成しました。送信中です…"
        ble.sendVJFrames(frames: generated)
    }
}
