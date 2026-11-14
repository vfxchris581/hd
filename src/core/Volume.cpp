#include "Volume.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hd {

// ---------------------------------------------------------------- thread pool
namespace {
class Pool {
public:
    Pool() {
        unsigned hc = std::thread::hardware_concurrency();
        int workers = int(std::min(16u, hc > 1 ? hc - 1 : 0u));
        for (int i = 0; i < workers; ++i) std::thread([this] { work(); }).detach();
        workers_ = workers;
    }
    void run(int n, const std::function<void(int, int)>& fn) {
        if (n <= 0) return;
        std::unique_lock<std::mutex> runLock(runMutex_, std::try_to_lock);
        if (workers_ == 0 || n < 4 || !runLock.owns_lock()) { fn(0, n); return; } // nested or tiny: run inline
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &fn;
            n_ = n;
            chunk_ = std::max(1, n / ((workers_ + 1) * 4));
            next_.store(0);
            active_ = workers_;
            ++generation_;
        }
        cvWork_.notify_all();
        drain(fn, n_, chunk_);
        std::unique_lock<std::mutex> lk(m_);
        cvDone_.wait(lk, [this] { return active_ == 0; });
        job_ = nullptr;
    }

private:
    void drain(const std::function<void(int, int)>& fn, int n, int chunk) {
        for (;;) {
            int b = next_.fetch_add(chunk);
            if (b >= n) break;
            fn(b, std::min(n, b + chunk));
        }
    }
    void work() {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int, int)>* job;
            int n, chunk;
            {
                std::unique_lock<std::mutex> lk(m_);
                cvWork_.wait(lk, [&] { return generation_ != seen; });
                seen = generation_;
                job = job_; n = n_; chunk = chunk_;
            }
            drain(*job, n, chunk);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--active_ == 0) cvDone_.notify_all();
            }
        }
    }
    std::mutex m_, runMutex_;
    std::condition_variable cvWork_, cvDone_;
    const std::function<void(int, int)>* job_ = nullptr;
    int n_ = 0, chunk_ = 1, active_ = 0, workers_ = 0;
    std::atomic<int> next_{0};
    uint64_t generation_ = 0;
};
} // namespace

void parallelFor(int n, const std::function<void(int, int)>& fn) {
    static Pool* pool = new Pool(); // intentionally leaked: workers idle until process exit
    pool->run(n, fn);
}

// ---------------------------------------------------------------- voxelisation
static size_t stampSpheres(const Geometry& g, int nx, int ny, int nz, const Vec3& origin, float dx, float radius,
                           std::vector<unsigned char>& mask) {
    const Attrib* ps = g.findAttrib(AttribClass::Point, "pscale");
    size_t count = 0;
    for (size_t pi = 0; pi < g.numPoints(); ++pi) {
        float r = std::max(ps ? ps->getF(pi) : radius, dx * 0.75f) / dx;
        Vec3 c = (g.P[pi] - origin) / dx;
        int i0 = std::max(0, int(std::floor(c.x - r))), i1 = std::min(nx - 1, int(std::ceil(c.x + r)));
        int j0 = std::max(0, int(std::floor(c.y - r))), j1 = std::min(ny - 1, int(std::ceil(c.y + r)));
        int k0 = std::max(0, int(std::floor(c.z - r))), k1 = std::min(nz - 1, int(std::ceil(c.z + r)));
        for (int k = k0; k <= k1; ++k)
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    Vec3 d = Vec3(i + 0.5f, j + 0.5f, k + 0.5f) - c;
                    if (dot(d, d) > r * r) continue;
                    unsigned char& m = mask[size_t(i) + size_t(nx) * (size_t(j) + size_t(ny) * size_t(k))];
                    if (!m) { m = 1; ++count; }
                }
    }
    return count;
}

size_t voxelizeGeometry(const Geometry& g, int nx, int ny, int nz, const Vec3& origin, float dx, float pointRadius,
                        std::vector<unsigned char>& mask) {
    mask.assign(size_t(nx) * size_t(ny) * size_t(nz), 0);
    if (nx <= 0 || ny <= 0 || nz <= 0) return 0;
    bool hasPolys = false;
    for (auto& pr : g.prims) if (pr.closed && pr.count >= 3) { hasPolys = true; break; }
    if (!hasPolys) return stampSpheres(g, nx, ny, nz, origin, dx, pointRadius, mask);

    // Cast a ray up every (x, z) column, collect surface crossings, fill between pairs.
    std::vector<std::vector<float>> cols(size_t(nx) * size_t(nz));
    const float ox = 0.5f + 0.00013f, oz = 0.5f + 0.00029f; // nudge samples off exact edges
    for (auto& pr : g.prims) {
        if (!pr.closed || pr.count < 3) continue;
        Vec3 a = (g.P[size_t(g.vertices[size_t(pr.start)])] - origin) / dx;
        for (int t = 1; t + 1 < pr.count; ++t) {
            Vec3 b = (g.P[size_t(g.vertices[size_t(pr.start + t)])] - origin) / dx;
            Vec3 c = (g.P[size_t(g.vertices[size_t(pr.start + t + 1)])] - origin) / dx;
            float area = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
            if (std::fabs(area) < 1e-12f) continue;
            int i0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}) - 1))), i1 = std::min(nx - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
            int k0 = std::max(0, int(std::floor(std::min({a.z, b.z, c.z}) - 1))), k1 = std::min(nz - 1, int(std::ceil(std::max({a.z, b.z, c.z}))));
            for (int k = k0; k <= k1; ++k)
                for (int i = i0; i <= i1; ++i) {
                    float px = i + ox, pz = k + oz;
                    float w0 = ((b.x - px) * (c.z - pz) - (b.z - pz) * (c.x - px)) / area;
                    float w1 = ((c.x - px) * (a.z - pz) - (c.z - pz) * (a.x - px)) / area;
                    float w2 = 1.f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    cols[size_t(i) + size_t(nx) * size_t(k)].push_back(w0 * a.y + w1 * b.y + w2 * c.y);
                }
        }
    }
    size_t count = 0;
    for (int k = 0; k < nz; ++k)
        for (int i = 0; i < nx; ++i) {
            auto& ys = cols[size_t(i) + size_t(nx) * size_t(k)];
            if (ys.size() < 2) continue;
            std::sort(ys.begin(), ys.end());
            for (size_t q = 0; q + 1 < ys.size(); q += 2) {
                int j0 = std::max(0, int(std::ceil(ys[q] - 0.5f))), j1 = std::min(ny - 1, int(std::floor(ys[q + 1] - 0.5f)));
                for (int j = j0; j <= j1; ++j) {
                    unsigned char& m = mask[size_t(i) + size_t(nx) * (size_t(j) + size_t(ny) * size_t(k))];
                    if (!m) { m = 1; ++count; }
                }
            }
        }
    if (count == 0) return stampSpheres(g, nx, ny, nz, origin, dx, pointRadius, mask); // mesh thinner than a voxel
    return count;
}

} // namespace hd
