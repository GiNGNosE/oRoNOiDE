# 決定 4: Entity管理 + ID体系 + Version Parity

**議論ファイル**: なし（本会話で直接議論）

---

## 設計方針

> **汎用ECS不採用。均質なEntity構造に特化したシンプルなEntity管理 + 即時再計算。**

全EntityがStone（石/岩/破片）であり、同一のフィールドを持つ。汎用ECSのアーキタイプ管理やComponent付け外し機構は不要。密配列 + 線形走査が最も効率的。

---

## 汎用ECS不採用の根拠

汎用ECSフレームワーク（EnTT, flecs等）が解決する問題:
- 何千種類のEntityに任意のComponentを動的に付け外し
- Componentの組み合わせに基づくSystemディスパッチ
- CPU側データレイアウト最適化

このプロジェクトでは:
- Entity種類が1つ（石）— Component組み合わせの爆発がない
- パフォーマンスクリティカルな処理はGPU上 — CPU側レイアウト最適化の恩恵が薄い
- System数が少ない（物理、レンダリング、破壊、導出キャッシュ程度）

汎用ECSの複雑さに対して得られるものが少ない。

---

## Entity構造

```cpp
struct StoneEntity {
    EntityId id;

    // 永続 (63B) — セーブ対象
    uint32_t topology_version;         // SDF変更時にインクリメント
    float16_t temperature;             // 石全体の基本温度
    vec3h layering_orientation;        // 堆積層方向
    float16_t grain_size_modifier;     // LUT粒径への乗算係数
    mat3x4f generation_transform;      // 生成時のワールド変換
    uint8_t wetness;                   // 濡れ度

    // 導出キャッシュ (40B) — topology_version変更時に再計算
    float mass;
    vec3f center_of_mass;
    SymMat3f inertia_tensor;           // 対称行列の上三角6成分

    // SBPへの参照
    BrickSet owned_bricks;             // 所有ブリックのインデックスリスト（導出キャッシュ）
};
```

### スクリプト言語について

Lua等のスクリプト層はエンジンコアの上に後から追加可能。ゲームプレイロジック（配置ルール、クラフトレシピ等）の需要が明確になった時点で導入を判断する。現段階では不要。

---

## Entity ID: Generational Index

```
EntityId = slot_index (24bit) + generation (8bit) = 32bit

slot_index: 密配列内の位置。削除後に再利用可能
generation: スロット再利用時にインクリメント（ABA問題の防止）
```

- 16M Entityまで対応
- 世代は256回で一巡するが実用上十分
- 古いIDでのアクセスは世代不一致で無効と判定

```cpp
class EntityManager {
    std::vector<StoneEntity> entities;   // 密配列
    std::vector<uint8_t> generations;    // ID再利用時のABA防止
    FreeList free_slots;

    EntityId create();                   // 新規ID発行
    void destroy(EntityId id);           // スロット解放 + generation++
    StoneEntity* get(EntityId id);       // 世代チェック付きアクセス
};
```

### 破壊分裂時のID管理

```
石A (id=100) が3つに割れた場合:

  親: id=100 → 最大破片が継承（topology_version++）
  子1: id=new1 → 新規ID発行
  子2: id=new2 → 新規ID発行

  子のgeneration_transform = 親から継承
  子のtopology_version = 1（新規）
```

最大破片が親IDを継承する理由: 外部参照（プレイヤーが持っている石等）が破壊後もなるべく有効であるため。

### 親子関係トラッキング

現時点では不採用。破片の来歴を辿る明確なゲームプレイ需要がない。必要になった場合、per-Entityに`parent_id: EntityId (4B)`を追加するだけで対応可能。設計への影響はない。

---

## Entity ↔ SBPブリック所有関係

### 双方向参照

**ブリック → Entity（権威的）:**

```
brick_header（既存フィールドに追加）:
  owner_id: EntityId (4B)
```

- レイヒット時にどの石か判定（ピッキング、衝突応答）
- 破壊分裂時: owner_idを書き換えるだけ

**Entity → ブリック（導出キャッシュ）:**

per-EntityのBrickSet（所有ブリックのインデックスリスト）。全ブリック走査（mass計算等）時のフィルタリング回避。

ブリックヘッダーのowner_idが権威的データ。BrickSetはtopology_version変更時にowner_idから再構築する導出キャッシュとして扱う。

---

## Version Parity: 即時再計算

### topology_version変更時の処理

```cpp
void on_topology_changed(StoneEntity& e) {
    e.topology_version++;
    recompute_brick_set(e);          // owner_idからBrickSet再構築
    recompute_derived_cache(e);      // mass, center_of_mass, inertia_tensor
    mark_lod_dirty(e);               // LOD再集約をキューに入れる
}
```

per-Systemのversion追跡は採用しない。System数が少なく、Entity構造が均質であるため、即時再計算の方がシンプルで十分。遅延が必要になった場合に初めてper-System version追跡を検討する。

### topology_versionが変わる契機

| イベント | 発生源 |
|---------|--------|
| 破壊コミット | 破壊ソルバー（phase_field_d=1 → SDF更新） |
| 彫刻/研磨 | プレイヤーアクション |
| 破壊分裂 | SDF連結成分分析 → Entity分割 |

---

## Entityライフサイクル

### 生成

```
1. ワールド生成
   EntityManager.create()
   → SBPにブリック確保 → フィールド生成
   → on_topology_changed()

2. 破壊分裂
   SDF連結成分分析
   → 最大破片: 親ID継承 + on_topology_changed()
   → 残り: EntityManager.create() + ブリックowner_id書き換え
   → 各子にon_topology_changed()

3. プレイヤー設置
   EntityManager.create()
   → ブリック確保 → フィールド設定
   → on_topology_changed()
```

### 消滅

```
1. 粉砕
   残存ボクセル数が閾値以下（目に見えないサイズ）
   → 粉塵パーティクルに変換
   → 所有ブリック全解放
   → EntityManager.destroy(id)

2. デスポーン（距離/メモリ圧による退避）
   ブリックをディスクに退避 + Entityメタデータ永続化
   → EntityManager.destroy(id)（メモリ解放のみ、データはディスク）

3. リスポーン（デスポーンの逆）
   ディスクからブリック復帰
   → EntityManager.create()（新ID発行）
   → 導出キャッシュ再計算
```

---

## メインループ

```cpp
void tick(float dt) {
    input_system.update();                    // プレイヤー入力
    physics_system.update(entities, dt);       // 剛体物理（SDF直接衝突判定）
    fracture_system.update(entities);          // 破壊判定 → ソルバー起動/コミット
    // on_topology_changed() は破壊コミット時に即時呼ばれる
    despawn_system.update(entities);           // デスポーン/リスポーン判定
    render_system.submit(entities);            // PTレンダリング
}
```

---

## 他の決定への依存関係

```
決定 4 (本決定)
  |
  +---> 決定 1a: SBPブリックヘッダーにowner_id (4B) 追加
  |
  +---> 決定 2: per-Entityメタデータ (63B永続 + 40B導出) がStoneEntityの構造
  |     topology_versionが導出キャッシュ再計算のトリガー
  |
  +---> 決定 3: SDF直接物理エンジンがphysics_systemの基盤
  |     レイヒット時のowner_idでEntity特定
  |
  +---> 第3章 (破壊): 破壊コミット → on_topology_changed() → Entity分裂
  |     最大破片が親ID継承
  |
  +---> 第5章 (永続化): EntityメタデータとSBPブリックのセーブ/ロード
  |     デスポーン/リスポーンのディスク退避設計
```
