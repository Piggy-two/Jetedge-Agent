// jetedge_server — JetEdge-Agent main entry point (Stage 2).
//
// Usage:
//   jetedge_server <streams.yaml>
//
// The YAML file lists one or more video sources and mux settings.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <glib-unix.h>
#include <gst/gst.h>

#include "jetedge/common/config_loader.h"
#include "jetedge/common/logging.h"
#include "jetedge/common/secrets.h"
#include "jetedge/pipeline/pipeline.h"

namespace {

constexpr const char* kAppName    = "jetedge_server";
constexpr const char* kAppVersion = "0.3.0";

jetedge::pipeline::Pipeline* g_pipeline = nullptr;

gboolean on_signal(gpointer /*user_data*/) {
  LOG_INFO("main", "received SIGINT/SIGTERM, initiating graceful shutdown...");
  if (g_pipeline) g_pipeline->quit();
  return G_SOURCE_REMOVE;
}

void print_usage(const char* prog) {
  std::fprintf(stderr, "Usage: %s <streams.yaml>\n", prog);
  std::fprintf(stderr, "  The YAML config specifies source URIs and mux settings.\n");
  std::fprintf(stderr, "  See configs/streams.yaml for an example.\n");
}

}  // namespace

int main(int argc, char* argv[]) {
  // ---- Parse arguments ----------------------------------------------------
  if (argc != 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }
  const char* config_path = argv[1];
  if (std::strcmp(config_path, "--help") == 0 || std::strcmp(config_path, "-h") == 0) {
    print_usage(argv[0]);
    return EXIT_SUCCESS;
  }

  // ---- Load configuration -------------------------------------------------
  jetedge::common::StreamsConfig config;
  std::string error;
  if (!jetedge::common::load_streams_config(config_path, config, error)) {
    std::fprintf(stderr, "error | main | config parse failed: %s\n", error.c_str());
    return EXIT_FAILURE;
  }

  if (config.streams.empty()) {
    std::fprintf(stderr, "error | main | no streams configured\n");
    return EXIT_FAILURE;
  }

  // ---- Load secrets (env vars or ~/.jetedge/secrets.env) -------------------
  // API keys are never logged; a missing key only disables that provider.
  jetedge::common::load_secrets_file();

  // ---- Initialize GStreamer -----------------------------------------------
  if (!gst_init_check(&argc, &argv, nullptr)) {
    std::fprintf(stderr, "error | main | gst_init_check failed\n");
    return EXIT_FAILURE;
  }

  LOG_INFO("main", "%s v%s starting with %zu stream(s)",
           kAppName, kAppVersion, config.streams.size());
  for (const auto& s : config.streams) {
    LOG_INFO("main", "  stream %s: uri=%s priority=%s",
             s.id.c_str(), s.uri.c_str(), priority_str(s.priority));
  }

  // ---- Signal handlers ----------------------------------------------------
  guint sigint_id  = g_unix_signal_add(SIGINT,  on_signal, nullptr);
  guint sigterm_id = g_unix_signal_add(SIGTERM, on_signal, nullptr);

  // ---- Build and run pipeline ----------------------------------------------
  jetedge::pipeline::Pipeline pipeline;
  g_pipeline = &pipeline;

  if (!pipeline.build(config.streams, config.mux, config.inference,
                      config.tracker, config.output, config.events,
                      config.llm, config.rtsp)) {
    LOG_ERROR("main", "", "init", "BUILD010", "%s", "pipeline build failed");
    gst_deinit();
    return EXIT_FAILURE;
  }

  pipeline.run();

  // ---- Report --------------------------------------------------------------
  pipeline.print_stats();

  // ---- Cleanup ------------------------------------------------------------
  g_pipeline = nullptr;
  (void)sigint_id;
  (void)sigterm_id;
  gst_deinit();

  LOG_INFO("main", "exit OK");
  return EXIT_SUCCESS;
}
