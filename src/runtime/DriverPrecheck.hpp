#pragma once
// 姿势驱动器的**必需引用预检**：纯逻辑，不碰 il2cpp，可以离线测。
//
// 为什么单独拆出来：旧写法是先 `AddComponent`、再逐项写引用，缺哪一项只 warn 后 continue ——
// prefab 上于是留下一个半初始化的组件。它照样被 Instantiate、照样 OnEnable，然后带着空引用
// 参与解算，而日志里只有一行 warn。这类"日志说没成、组件却在跑"的洞比不挂更坏，所以改成
// **挂之前先查，缺任一项整体拒绝**。
//
// 判断"字段在不在""骨找不找得到"都由调用方注入，于是这段决策逻辑可以在离线测试里两个方向
// 都验（坏 sidecar 必须报、正常包不许误报），不需要进游戏。

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GakumasMod::Runtime {

    // P3：给**运行时新建的衣物骨**装学马自己的姿势驱动器（Skirt / Waist / Frill / Poncho /
    // Furisode / HumanoidSleeve / Rotation ……）。
    //
    // 字段按**名字**写，不按偏移：一是「别猜偏移」是这个项目反复付过学费的规矩；二是 12 种
    // setting 各有各的字段，硬编码偏移等于把 il2cpp 布局钉死在运行时里，游戏一更新就集体错位。
    // 名字由导出器从原版 bundle 的内嵌 typetree 里抄；类型不靠猜 —— sidecar 分四张表明说是
    // int / float / float3 / 骨引用。
    struct LocalQuartzDriver {
        std::string type;                                        // 组件类名去掉前后缀，如 "Skirt"
        std::unordered_map<std::string, int> ints;
        std::unordered_map<std::string, float> floats;
        std::unordered_map<std::string, std::array<float, 3>> vectors;
        std::unordered_map<std::string, std::string> bones;       // 字段名 → 目标骨名
    };

    // 返回缺失项的可读清单（settingName.字段 / → 骨 名字）。空 = 可以挂。
    inline std::vector<std::string> MissingDriverReferences(
        const std::string& settingName,
        const LocalQuartzDriver& driver,
        const std::function<bool(const std::string&)>& fieldExists,
        const std::function<bool(const std::string&)>& boneExists) {
        std::vector<std::string> missing;
        const auto requireField = [&](const std::string& name) {
            if (!fieldExists || !fieldExists(name)) {
                missing.push_back(settingName + "." + name + "（字段不存在）");
            }
        };
        for (const auto& [name, _value] : driver.ints) requireField(name);
        for (const auto& [name, _value] : driver.floats) requireField(name);
        for (const auto& [name, _value] : driver.vectors) requireField(name);
        for (const auto& [name, boneName] : driver.bones) {
            requireField(name);
            if (!boneExists || !boneExists(boneName)) {
                missing.push_back(settingName + "." + name + " → 骨 " + boneName + "（找不到）");
            }
        }
        return missing;
    }

    inline std::string JoinMissingReferences(const std::vector<std::string>& missing) {
        std::string detail;
        for (const auto& item : missing) {
            if (!detail.empty()) detail += "、";
            detail += item;
        }
        return detail;
    }

}  // namespace GakumasMod::Runtime
