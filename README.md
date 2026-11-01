# PhotoDongle — BLE写真ドングル

ESP32-S3-0.96-SD（Spotpear/GNPE、0.96インチ 160×80 LCD搭載）に、iPhoneから
Bluetooth LEで写真を送って表示する仕組み。スライドショー表示に加えて、
写真から生成したアニメーションをループ再生する「VJモード」もある。

## 構成

| フォルダ | 中身 |
|---|---|
| `PhotoDongle/` + `PhotoDongle.xcodeproj/` | iPhoneアプリ（SwiftUI）。写真を選んで160×80 RGB565へ変換しBLE送信。複数枚の一括送信、拡大時の切り抜き位置指定、VJフレーム生成（CoreImage） |
| `esp32-s3-photo-dongle/` | ドングル側ファームウェア（ESP-IDF / NimBLE）。受信した写真をmicroSDへ保存してスライドショー表示、VJフレームのループ再生 |

## 使い方（ざっくり）

1. ファームウェアをESP-IDFでビルドしてドングルへ書き込む（詳細は `esp32-s3-photo-dongle/README.md`）
2. FAT32のmicroSDを挿してドングルを起動（BLE名「PHOTO-DONGLE」）
3. iPhoneアプリから接続して写真を送信

## BLEプロトコル

カスタムGATTサービス 0xFFF0（CTRL=FFF1 / DATA=FFF2 / STAT=FFF3）。
フレーム形式は160×80・RGB565ビッグエンディアン・25600バイト固定。
コマンドの詳細はファームウェア側READMEと `esp32-s3-photo-dongle/main/main.c` の
ヘッダコメントを参照。

## バージョン

- ファームウェア v1.1（画面左上に版数を常時表示）
