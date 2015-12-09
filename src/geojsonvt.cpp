#include <mapbox/geojsonvt.hpp>
#include <mapbox/geojsonvt/clip.hpp>
#include <mapbox/geojsonvt/convert.hpp>
#include <mapbox/geojsonvt/wrap.hpp>

#include <stack>
#include <cmath>
#include <unordered_map>

namespace mapbox {
namespace geojsonvt {

#pragma mark - GeoJSONVT

std::unordered_map<std::string, clock_t> activities;

void time(std::string activity) {
    activities[activity] = clock();
}

void timeEnd(std::string activity) {
    printf("%s: %fms\n", activity.c_str(),
           double(clock() - activities[activity]) / (CLOCKS_PER_SEC / 1000));
}

const Tile GeoJSONVT::emptyTile{};

GeoJSONVT::GeoJSONVT(std::vector<ProjectedFeature> features_, Options options_)
    : options(std::move(options_)) {

#ifdef DEBUG
    printf("index: maxZoom: %d, maxPoints: %d", options.indexMaxZoom, options.indexMaxPoints);
    time("generate tiles");
#endif

    features_ = Wrap::wrap(features_, double(options.buffer) / options.extent, intersectX);

    // start slicing from the top tile down
    if (!features_.empty()) {
        splitTile(features_, 0, 0, 0);
    }

#ifdef DEBUG
    if (!features_.empty()) {
        printf("features: %i, points: %i\n", tiles[0].numFeatures, tiles[0].numPoints);
    }
    timeEnd("generate tiles");
    printf("tiles generated: %i {\n", static_cast<int>(total));
    for (const auto& pair : stats) {
        printf("    z%i: %i\n", pair.first, pair.second);
    }
    printf("}\n");
#endif
}

const Tile& GeoJSONVT::getTile(uint8_t z, uint32_t x, uint32_t y) {
    const uint32_t z2 = 1 << z;
    x = ((x % z2) + z2) % z2; // wrap tile x coordinate

    const uint64_t id = toID(z, x, y);
    if (tiles.count(id) != 0u) {
        return transformTile(tiles.find(id)->second, options.extent);
    }

#ifdef DEBUG
    printf("drilling down to z%i-%i-%i\n", z, x, y);
#endif

    uint8_t z0 = z;
    uint32_t x0 = x;
    uint32_t y0 = y;
    Tile* parent = nullptr;

    while ((parent == nullptr) && (z0 != 0u)) {
        z0--;
        x0 = x0 / 2;
        y0 = y0 / 2;
        const uint64_t checkID = toID(z0, x0, y0);
        if (tiles.count(checkID) != 0u) {
            parent = &tiles[checkID];
        }
    }

    if (parent == nullptr) {
        return emptyTile;
    }

#ifdef DEBUG
    printf("found parent tile z%i-%i-%i\n", z0, x0, y0);
#endif

    // if we found a parent tile containing the original geometry, we can drill down from it
    if ((parent != nullptr) && !parent->source.empty()) {
        if (isClippedSquare(*parent, options.extent, options.buffer)) {
            return transformTile(*parent, options.extent);
        }

#ifdef DEBUG
        time("drilling down");
#endif

        splitTile(parent->source, z0, x0, y0, z, x, y);

#ifdef DEBUG
        timeEnd("drilling down");
#endif
    }

    if (tiles.find(id) == tiles.end()) {
        return emptyTile;
    }

    return transformTile(tiles[id], options.extent);
}

const std::map<uint64_t, Tile>& GeoJSONVT::getAllTiles() const {
    return tiles;
}

uint64_t GeoJSONVT::getTotal() const {
    return total;
}

std::vector<ProjectedFeature> GeoJSONVT::convertFeatures(const std::string& data, Options options) {
#ifdef DEBUG
    time("preprocess data");
#endif

    uint32_t z2 = 1 << options.maxZoom; // 2^z

    rapidjson::Document deserializedData;
    deserializedData.Parse<0>(data.c_str());

    if (deserializedData.HasParseError()) {
        throw std::runtime_error("Invalid GeoJSON");
    }

    std::vector<ProjectedFeature> features =
        Convert::convert(deserializedData, options.tolerance / (z2 * options.extent));

#ifdef DEBUG
    timeEnd("preprocess data");
#endif

    return features;
}

void GeoJSONVT::splitTile(std::vector<ProjectedFeature> features_,
                          uint8_t z_,
                          uint32_t x_,
                          uint32_t y_,
                          uint8_t cz,
                          uint32_t cx,
                          uint32_t cy) {
    std::stack<FeatureStackItem> stack;
    stack.emplace(features_, z_, x_, y_);

    while (!stack.empty()) {
        FeatureStackItem set = stack.top();
        stack.pop();
        std::vector<ProjectedFeature> features = std::move(set.features);
        uint8_t z = set.z;
        uint32_t x = set.x;
        uint32_t y = set.y;

        uint32_t z2 = 1 << z;
        const uint64_t id = toID(z, x, y);
        Tile* tile = [&]() {
            const auto it = tiles.find(id);
            return it != tiles.end() ? &it->second : nullptr;
        }();
        double tileTolerance =
            (z == options.maxZoom ? 0 : options.tolerance / (z2 * options.extent));

        if (tile == nullptr) {
#ifdef DEBUG
            time("creation");
#endif

            tiles[id] = std::move(
                Tile::createTile(features, z2, x, y, tileTolerance, (z == options.maxZoom)));
            tile = &tiles[id];

#ifdef DEBUG
            printf("tile z%i-%i-%i (features: %i, points: %i, simplified: %i\n", z, x, y,
                   tile->numFeatures, tile->numPoints, tile->numSimplified);
            timeEnd("creation");

            uint8_t key = z;
            stats[key] = (stats.count(key) ? stats[key] + 1 : 1);
#endif

            total++;
        }

        // save reference to original geometry in tile so that we can drill down later if we stop
        // now
        tile->source = std::vector<ProjectedFeature>(features);

        // stop tiling if the tile is solid clipped square
        if (!options.solidChildren && isClippedSquare(*tile, options.extent, options.buffer)) {
            continue;
        }

        // if it's the first-pass tiling
        if (cz == 0u) {
            // stop tiling if we reached max zoom, or if the tile is too simple
            if (z == options.indexMaxZoom || tile->numPoints <= options.indexMaxPoints) {
                continue;
            }

            // if a drilldown to a specific tile
        } else {
            // stop tiling if we reached base zoom or our target tile zoom
            if (z == options.maxZoom || z == cz) {
                continue;
            }

            // stop tiling if it's not an ancestor of the target tile
            const auto m = 1 << (cz - z);
            if (x != std::floor(cx / m) || y != std::floor(cy / m)) {
                continue;
            }
        }

        // if we slice further down, no need to keep source geometry
        tile->source = {};

#ifdef DEBUG
        time("clipping");
#endif

        const double k1 = 0.5 * options.buffer / options.extent;
        const double k2 = 0.5 - k1;
        const double k3 = 0.5 + k1;
        const double k4 = 1 + k1;

        std::vector<ProjectedFeature> tl;
        std::vector<ProjectedFeature> bl;
        std::vector<ProjectedFeature> tr;
        std::vector<ProjectedFeature> br;

        const auto left =
            Clip::clip(features, z2, x - k1, x + k3, 0, intersectX, tile->min.x, tile->max.x);
        const auto right =
            Clip::clip(features, z2, x + k2, x + k4, 0, intersectX, tile->min.x, tile->max.x);

        if (!left.empty()) {
            tl = Clip::clip(left, z2, y - k1, y + k3, 1, intersectY, tile->min.y, tile->max.y);
            bl = Clip::clip(left, z2, y + k2, y + k4, 1, intersectY, tile->min.y, tile->max.y);
        }

        if (!right.empty()) {
            tr = Clip::clip(right, z2, y - k1, y + k3, 1, intersectY, tile->min.y, tile->max.y);
            br = Clip::clip(right, z2, y + k2, y + k4, 1, intersectY, tile->min.y, tile->max.y);
        }

#ifdef DEBUG
        timeEnd("clipping");
#endif

        if (!tl.empty()) {
            stack.emplace(std::move(tl), z + 1, x * 2, y * 2);
        }
        if (!bl.empty()) {
            stack.emplace(std::move(bl), z + 1, x * 2, y * 2 + 1);
        }
        if (!tr.empty()) {
            stack.emplace(std::move(tr), z + 1, x * 2 + 1, y * 2);
        }
        if (!br.empty()) {
            stack.emplace(std::move(br), z + 1, x * 2 + 1, y * 2 + 1);
        }
    }
}

TilePoint GeoJSONVT::transformPoint(
    const ProjectedPoint& p, uint16_t extent, uint32_t z2, uint32_t tx, uint32_t ty) {

    int16_t x = std::round(extent * (p.x * z2 - tx));
    int16_t y = std::round(extent * (p.y * z2 - ty));

    return TilePoint(x, y);
}

const Tile& GeoJSONVT::transformTile(Tile& tile, uint16_t extent) {
    if (tile.transformed) {
        return tile;
    }

    const uint32_t z2 = tile.z2;
    const uint32_t tx = tile.tx;
    const uint32_t ty = tile.ty;

    for (auto& feature : tile.features) {
        const auto& geom = feature.geometry;
        const auto type = feature.type;

        if (type == TileFeatureType::Point) {
            auto& tileGeom = feature.tileGeometry.get<TilePoints>();
            for (const auto& pt : geom.get<ProjectedPoints>()) {
                tileGeom.push_back(transformPoint(pt, extent, z2, tx, ty));
            }

        } else {
            feature.tileGeometry.set<TileRings>();
            auto& tileGeom = feature.tileGeometry.get<TileRings>();
            for (const auto& r : geom.get<ProjectedRings>()) {
                TilePoints ring;
                for (const auto& p : r.points) {
                    ring.push_back(transformPoint(p, extent, z2, tx, ty));
                }
                tileGeom.push_back(std::move(ring));
            }
        }
    }

    tile.transformed = true;

    return tile;
}

uint64_t GeoJSONVT::toID(uint8_t z, uint32_t x, uint32_t y) {
    return (((1 << z) * y + x) * 32) + z;
}

ProjectedPoint GeoJSONVT::intersectX(const ProjectedPoint& a, const ProjectedPoint& b, double x) {
    double y = (x - a.x) * (b.y - a.y) / (b.x - a.x) + a.y;
    return ProjectedPoint(x, y, 1.0);
}

ProjectedPoint GeoJSONVT::intersectY(const ProjectedPoint& a, const ProjectedPoint& b, double y) {
    double x = (y - a.y) * (b.x - a.x) / (b.y - a.y) + a.x;
    return ProjectedPoint(x, y, 1.0);
}

// checks whether a tile is a whole-area fill after clipping; if it is, there's no sense slicing it
// further
bool GeoJSONVT::isClippedSquare(Tile& tile, const uint16_t extent, const uint8_t buffer) {
    const auto& features = tile.source;
    if (features.size() != 1) {
        return false;
    }

    const auto& feature = features.front();
    if (feature.type != ProjectedFeatureType::Polygon) {
        return false;
    }
    const auto& rings = feature.geometry.get<ProjectedRings>();
    if (rings.size() > 1) {
        return false;
    }

    const auto& ring = rings.front();

    if (ring.points.size() != 5) {
        return false;
    }

    for (const auto& pt : ring.points) {
        auto p = transformPoint(pt, extent, tile.z2, tile.tx, tile.ty);
        if ((p.x != -buffer && p.x != extent + buffer) ||
            (p.y != -buffer && p.y != extent + buffer)) {
            return false;
        }
    }

    return true;
}

} // namespace geojsonvt
} // namespace mapbox
