// Tiempo de GPU por cuadro, separado en dibujos, resolves y swap (cvar gpu_tiempos).
// Cada backend escribe marcas de tiempo (timestamps) en la GPU: inicio del cuadro, antes y despues
// de cada resolve (IssueCopy), inicio del swap y fin del cuadro. Cuando la GPU termina ese cuadro
// se leen y se acumulan; cada 5 s se escribe una linea "[gpu]" en el log.
//   total    = fin - inicio (incluye huecos si el CPU no alimenta a la GPU)
//   resolves = suma de (fin - inicio) de cada resolve
//   swap     = fin - inicio del swap (gamma/FXAA de salida; el CAS del presentador no entra)
//   dibujos  = total - resolves - swap
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, gpu_tiempos);

namespace rex::graphics::gpu_tiempos {

enum Marca : uint8_t { kInicio, kResolveIni, kResolveFin, kSwapIni, kFin };

constexpr uint32_t kMarcasPorCuadro = 512;

struct Cuadro {
  uint32_t n = 0;
  uint8_t tipo[kMarcasPorCuadro] = {};
  // Se puede agregar la marca m? Siempre se deja lugar para la de fin.
  bool Cabe(Marca m) const {
    if (m == kInicio) return n == 0;
    if (n == 0) return false;
    if (m == kFin) return n < kMarcasPorCuadro && tipo[n - 1] != kFin;
    return n + 2 < kMarcasPorCuadro && tipo[n - 1] != kFin;
  }
  bool Completo() const { return n >= 2 && tipo[0] == kInicio && tipo[n - 1] == kFin; }
};

struct Acumulador {
  double total_ms = 0, dibujos_ms = 0, resolves_ms = 0, swap_ms = 0, max_total_ms = 0;
  uint32_t cuadros = 0, resolves = 0;
  std::chrono::steady_clock::time_point t0{};
  bool iniciado = false;

  void Procesar(const uint64_t* ts, const Cuadro& c, double ns_por_tick) {
    if (!c.Completo() || ns_por_tick <= 0) return;
    if (ts[c.n - 1] <= ts[0]) return;
    const double ms = ns_por_tick / 1e6;
    const double total = double(ts[c.n - 1] - ts[0]) * ms;
    double res = 0, swap = 0;
    uint64_t res_ini = 0;
    bool en_resolve = false;
    uint32_t n_res = 0;
    for (uint32_t i = 1; i + 1 < c.n; ++i) {
      if (c.tipo[i] == kResolveIni) {
        res_ini = ts[i];
        en_resolve = true;
      } else if (c.tipo[i] == kResolveFin && en_resolve) {
        if (ts[i] >= res_ini) res += double(ts[i] - res_ini) * ms;
        en_resolve = false;
        ++n_res;
      } else if (c.tipo[i] == kSwapIni) {
        if (ts[c.n - 1] >= ts[i]) swap = double(ts[c.n - 1] - ts[i]) * ms;
      }
    }
    total_ms += total;
    resolves_ms += res;
    swap_ms += swap;
    dibujos_ms += std::max(0.0, total - res - swap);
    max_total_ms = std::max(max_total_ms, total);
    resolves += n_res;
    ++cuadros;
  }

  void TalvezLog(const char* backend) {
    const auto now = std::chrono::steady_clock::now();
    if (!iniciado) {
      iniciado = true;
      t0 = now;
      return;
    }
    if (std::chrono::duration<double>(now - t0).count() < 5.0) return;
    if (cuadros) {
      const double c = double(cuadros);
      REXGPU_INFO(
          "[gpu] {} GPU por cuadro: total {:.2f} ms (peor {:.2f}) | dibujos {:.2f} ms | resolves "
          "{:.2f} ms ({:.1f}/cuadro) | swap {:.2f} ms | {} cuadros medidos",
          backend, total_ms / c, max_total_ms, dibujos_ms / c, resolves_ms / c, resolves / c,
          swap_ms / c, cuadros);
    }
    *this = Acumulador{};
    iniciado = true;
    t0 = now;
  }
};

}  // namespace rex::graphics::gpu_tiempos
