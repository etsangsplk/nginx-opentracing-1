#include "opentracing_handler.h"
#include <lightstep/tracer.h>
#include "opentracing_conf.h"
#include "opentracing_request_instrumentor.h"

extern "C" {
extern ngx_module_t ngx_http_opentracing_module;
}

namespace ngx_opentracing {
// Customization point: A tracer implementation needs to define this function
// that returns an instance of its specific tracer.
lightstep::Tracer make_tracer(const tracer_options_t &options);

//------------------------------------------------------------------------------
// make_tracer
//------------------------------------------------------------------------------
static lightstep::Tracer make_tracer(const ngx_http_request_t *request) {
  auto main_conf = static_cast<opentracing_main_conf_t *>(
      ngx_http_get_module_main_conf(request, ngx_http_opentracing_module));
  return make_tracer(main_conf->tracer_options);
}

//------------------------------------------------------------------------------
// OpenTracingContext
//------------------------------------------------------------------------------
namespace {
class OpenTracingContext {
 public:
  explicit OpenTracingContext(const ngx_http_request_t *request) {
    lightstep::Tracer::InitGlobal(make_tracer(request));
  }

  void on_enter_block(ngx_http_request_t *request);
  void on_log_request(ngx_http_request_t *request);

 private:
  std::unordered_map<const ngx_http_request_t *, OpenTracingRequestInstrumentor>
      active_instrumentors_;
};
}

//------------------------------------------------------------------------------
// get_handler_context
//------------------------------------------------------------------------------
static OpenTracingContext &get_handler_context(
    const ngx_http_request_t *request) {
  static OpenTracingContext handler_context{request};
  return handler_context;
}

//------------------------------------------------------------------------------
// is_opentracing_enabled
//------------------------------------------------------------------------------
static bool is_opentracing_enabled(
    const ngx_http_request_t *request,
    const ngx_http_core_loc_conf_t *core_loc_conf,
    const opentracing_loc_conf_t *loc_conf) {
  // Check if this is a main request instead of a subrequest.
  if (request == request->main)
    return loc_conf->enable;
  else
    // Only trace subrequests if `log_subrequest` is enabled; otherwise the
    // spans won't be finished.
    return loc_conf->enable && core_loc_conf->log_subrequest;
}

//------------------------------------------------------------------------------
// on_enter_block
//------------------------------------------------------------------------------
void OpenTracingContext::on_enter_block(ngx_http_request_t *request) {
  auto core_loc_conf = static_cast<ngx_http_core_loc_conf_t *>(
      ngx_http_get_module_loc_conf(request, ngx_http_core_module));
  auto loc_conf = static_cast<opentracing_loc_conf_t *>(
      ngx_http_get_module_loc_conf(request, ngx_http_opentracing_module));

  auto instrumentor_iter = active_instrumentors_.find(request);
  if (instrumentor_iter == active_instrumentors_.end()) {
    if (!is_opentracing_enabled(request, core_loc_conf, loc_conf)) return;
    active_instrumentors_.emplace(
        request,
        OpenTracingRequestInstrumentor{request, core_loc_conf, loc_conf});
  } else {
    instrumentor_iter->second.on_change_block(core_loc_conf, loc_conf);
  }
}

ngx_int_t on_enter_block(ngx_http_request_t *request) {
  get_handler_context(request).on_enter_block(request);
  return NGX_DECLINED;
}

//------------------------------------------------------------------------------
// on_log_request
//------------------------------------------------------------------------------
void OpenTracingContext::on_log_request(ngx_http_request_t *request) {
  auto instrumentor_iter = active_instrumentors_.find(request);
  if (instrumentor_iter == active_instrumentors_.end()) return;
  instrumentor_iter->second.on_log_request();
  active_instrumentors_.erase(instrumentor_iter);
}

ngx_int_t on_log_request(ngx_http_request_t *request) {
  get_handler_context(request).on_log_request(request);
  return NGX_DECLINED;
}
}  // namespace ngx_opentracing
