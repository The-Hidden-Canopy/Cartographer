#pragma once

#include <string_view>

// Generated from examples/material-libraries/starter-surfaces.gltf.
// The registered starter-library test verifies byte-for-byte parity.
namespace studio {
inline constexpr std::string_view kStarterMaterialLibrary = R"CARTO_STARTER({
  "asset": {
    "version": "2.0",
    "generator": "Cartographer starter material library"
  },
  "extras": {
    "cartographer": {
      "profile": "cartographer-gltf",
      "profile_version": 1
    }
  },
  "materials": [
    {
      "name": "Brushed Steel",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.48,
          0.5,
          0.52,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.38
      }
    },
    {
      "name": "Polished Steel",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.56,
          0.58,
          0.6,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.16
      }
    },
    {
      "name": "Dark Iron",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.2,
          0.21,
          0.22,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.65
      }
    },
    {
      "name": "Aluminum",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.62,
          0.64,
          0.66,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.32
      }
    },
    {
      "name": "Copper",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.72,
          0.34,
          0.19,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.35
      }
    },
    {
      "name": "Aged Copper",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.4,
          0.2,
          0.12,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.68
      }
    },
    {
      "name": "Brass",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.64,
          0.44,
          0.19,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.34
      }
    },
    {
      "name": "Gold",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.83,
          0.59,
          0.18,
          1
        ],
        "metallicFactor": 1,
        "roughnessFactor": 0.24
      }
    },
    {
      "name": "Matte Black Plastic",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.025,
          0.025,
          0.028,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.82
      }
    },
    {
      "name": "Satin Black Plastic",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.035,
          0.035,
          0.038,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.42
      }
    },
    {
      "name": "Ivory Plastic",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.72,
          0.69,
          0.6,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.5
      }
    },
    {
      "name": "Rubber",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.028,
          0.028,
          0.026,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.95
      }
    },
    {
      "name": "Terracotta",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.48,
          0.17,
          0.09,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.9
      }
    },
    {
      "name": "Concrete",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.42,
          0.41,
          0.38,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.94
      }
    },
    {
      "name": "Slate",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.16,
          0.19,
          0.21,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.88
      }
    },
    {
      "name": "Sandstone",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.58,
          0.45,
          0.3,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.94
      }
    },
    {
      "name": "Unfinished Oak",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.43,
          0.25,
          0.11,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.8
      }
    },
    {
      "name": "Dark Walnut",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.18,
          0.075,
          0.035,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.73
      }
    },
    {
      "name": "Canvas",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.6,
          0.55,
          0.43,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.98
      }
    },
    {
      "name": "Leather Brown",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.22,
          0.085,
          0.038,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.76
      }
    },
    {
      "name": "Painted Red",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.55,
          0.035,
          0.022,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.42
      }
    },
    {
      "name": "Painted Blue",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.025,
          0.12,
          0.38,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.42
      }
    },
    {
      "name": "Ceramic White",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.78,
          0.77,
          0.72,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.24
      }
    },
    {
      "name": "Ceramic Teal",
      "doubleSided": true,
      "pbrMetallicRoughness": {
        "baseColorFactor": [
          0.035,
          0.32,
          0.3,
          1
        ],
        "metallicFactor": 0,
        "roughnessFactor": 0.28
      }
    }
  ]
}
)CARTO_STARTER";
}
