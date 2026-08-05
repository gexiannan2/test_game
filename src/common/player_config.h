#pragma once

struct PlayerConfig {
    float capsule_height = 1.5f;
    float capsule_radius = 0.3f;

    static PlayerConfig& Instance() {
        static PlayerConfig inst;
        return inst;
    }

    void LoadDefaults() {
        capsule_height = 1.5f;
        capsule_radius = 0.3f;
    }
};
