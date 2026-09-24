// Explicit hardware qualification of command-buffer dependencies. No GPU work
// unless --run is passed. Distinct guarded buffers alternate
// source/destination; every producer is flushed separately without an
// intermediate host wait.
#include "hrx_compute_fixture.h"
#include "hrx_runtime.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void deadline(int signal) {
  (void)signal;
  _Exit(124);
}
static void must(hrx_status_t status, const char *name) {
  if (hrx_status_is_ok(status))
    return;
  fprintf(
      stderr,
      "FAIL %s status=%d; retaining potentially live resources until exit\n",
      name, hrx_status_code(status));
  char *message = NULL;
  size_t length = 0;
  hrx_status_t formatted = hrx_status_to_string(status, &message, &length);
  if (hrx_status_is_ok(formatted) && message)
    fprintf(stderr, "%.*s\n", (int)length, message);
  hrx_status_free_message(message);
  hrx_status_ignore(formatted);
  hrx_status_ignore(status);
  _Exit(2);
}
typedef struct {
  atomic_bool entered, released, finished;
} host_gate_t;
static hrx_status_t gated_host_call(void *opaque) {
  host_gate_t *gate = opaque;
  atomic_store_explicit(&gate->entered, true, memory_order_release);
  // Deliberately hold the real production notification callback. The main
  // thread releases it after two subsequent submissions; no runtime reentry.
  const struct timespec tick = {0, 1000000};
  for (unsigned i = 0;
       i < 5000 && !atomic_load_explicit(&gate->released, memory_order_acquire);
       ++i)
    nanosleep(&tick, NULL);
  if (!atomic_load_explicit(&gate->released, memory_order_acquire))
    return hrx_make_status(HRX_STATUS_DEADLINE_EXCEEDED,
                           "fixture host callback gate exceeded 5 seconds");
  atomic_store_explicit(&gate->finished, true, memory_order_release);
  return hrx_ok_status();
}
static hrx_status_t failing_host_call(void *unused) {
  (void)unused;
  return hrx_make_status(HRX_STATUS_ABORTED,
                         "intentional qualification callback failure");
}
int main(int argc, char **argv) {
  if (argc < 2 || strcmp(argv[1], "--run")) {
    puts("Usage: mac-hrx-dependency-test --run [--steps 2..65535] "
         "[--cross-queue | --host-action | --failure] (90s deadline)");
    return argc == 1 ? 0 : 2;
  }
  unsigned steps = 257;
  bool cross_queue = false, host_action = false, failure = false;
  for (int arg = 2; arg < argc; ++arg) {
    if (!strcmp(argv[arg], "--steps") && arg + 1 < argc) {
      char *end = NULL;
      errno = 0;
      unsigned long parsed = strtoul(argv[++arg], &end, 10);
      if (errno || *end || parsed < 2 || parsed > 65535)
        return 2;
      steps = (unsigned)parsed;
    } else if (!strcmp(argv[arg], "--cross-queue"))
      cross_queue = true;
    else if (!strcmp(argv[arg], "--host-action"))
      host_action = true;
    else if (!strcmp(argv[arg], "--failure"))
      failure = true;
    else
      return 2;
  }
  if ((unsigned)cross_queue + (unsigned)host_action + (unsigned)failure > 1)
    return 2;
  signal(SIGALRM, deadline);
  alarm(90);
  enum { N = 65533, G = 16, WORDS = N + 2 * G };
  const float guard = -8388607.0f;
  float *initial = malloc(WORDS * sizeof(float));
  float *ones = malloc(WORDS * sizeof(float));
  float *out = malloc(WORDS * sizeof(float));
  if (!initial || !ones || !out)
    return 2;
  for (unsigned i = 0; i < WORDS; ++i)
    initial[i] = ones[i] = guard;
  for (unsigned i = 0; i < N; ++i) {
    initial[i + G] = (float)(i % 127);
    ones[i + G] = 1.0f;
  }
  must(hrx_gpu_initialize(0), "initialize");
  hrx_device_t device = NULL;
  hrx_stream_t stream = NULL, peer = NULL;
  hrx_executable_t executable = NULL;
  hrx_buffer_t b[3] = {0};
  must(hrx_gpu_device_get(0, &device), "device");
  must(hrx_stream_create_on_queue(device, 0, 1, &stream),
       "pinned stream queue 0");
  uint32_t queue_count = 0;
  must(hrx_device_get_property(device, HRX_DEVICE_PROPERTY_QUEUE_COUNT,
                               &queue_count, sizeof(queue_count)),
       "queue count");
  if (cross_queue) {
    if (queue_count < 2) {
      fprintf(stderr, "FAIL two physical queues required; device reports %u\n",
              queue_count);
      _Exit(4);
    }
    must(hrx_stream_create_on_queue(device, 0, 2, &peer),
         "pinned stream queue 1");
  }
  if (failure) {
    hrx_semaphore_t failed = NULL;
    must(hrx_semaphore_create(device, 0, &failed), "failure semaphore");
    uint64_t value = 1;
    const hrx_semaphore_list_t signals = {&failed, &value, 1};
    must(
        hrx_queue_host_call(device, 1, NULL, &signals, failing_host_call, NULL),
        "submit failing callback");
    hrx_status_t result = hrx_semaphore_wait(failed, 1, 5000000000ull);
    if (hrx_status_code(result) != HRX_STATUS_ABORTED) {
      fprintf(stderr,
              "FAIL callback status propagation got %d expected ABORTED\n",
              hrx_status_code(result));
      char *detail = NULL;
      size_t length = 0;
      hrx_status_t formatted = hrx_status_to_string(result, &detail, &length);
      if (hrx_status_is_ok(formatted) && detail)
        fprintf(stderr, "%.*s\n", (int)length, detail);
      hrx_status_free_message(detail);
      hrx_status_ignore(formatted);
      hrx_status_ignore(result);
      _Exit(3);
    }
    hrx_status_ignore(result);
    hrx_semaphore_release(failed);
    hrx_stream_release(stream);
    must(hrx_gpu_shutdown(), "shutdown after callback failure");
    alarm(0);
    free(initial);
    free(ones);
    free(out);
    puts("PASS intentional host callback failure propagated ABORTED; clean "
         "shutdown");
    return 0;
  }
  for (unsigned i = 0; i < 3; ++i) {
    must(hrx_buffer_allocate(stream, WORDS * sizeof(float),
                             HRX_MEMORY_TYPE_DEVICE_LOCAL,
                             HRX_BUFFER_USAGE_DEFAULT, &b[i]),
         "allocate");
    must(hrx_synchronous_h2d(device, i == 2 ? ones : initial, b[i], 0,
                             WORDS * sizeof(float)),
         "upload");
  }
  must(hrx_executable_load_data(device, hrx_compute_fixture,
                                sizeof(hrx_compute_fixture), "amdgpu",
                                "gfx1201", &executable),
       "executable");
  uint32_t ordinal = 0;
  must(hrx_executable_lookup_export_by_name(executable, "hrx_vector_affine",
                                            &ordinal),
       "export");
  const struct {
    uint32_t count;
    float scale;
  } constants = {N, 1.0f};
  const hrx_dispatch_config_t config = {{(N + 63) / 64, 1, 1}, {64, 1, 1}, 32};
  host_gate_t gate = {0};
  hrx_semaphore_t host_done = NULL;
  if (host_action) {
    // Ensure allocation callbacks have retired before isolating the new action.
    must(hrx_stream_synchronize(stream), "retire setup before host action");
    must(hrx_semaphore_create(device, 0, &host_done), "host action semaphore");
    uint64_t value = 1;
    const hrx_semaphore_list_t signals = {&host_done, &value, 1};
    must(hrx_queue_host_call(device, 1, NULL, &signals, gated_host_call, &gate),
         "submit gated host callback A");
    const struct timespec tick = {0, 1000000};
    for (unsigned i = 0;
         i < 2000 && !atomic_load_explicit(&gate.entered, memory_order_acquire);
         ++i)
      nanosleep(&tick, NULL);
    if (!atomic_load_explicit(&gate.entered, memory_order_acquire)) {
      fprintf(stderr, "FAIL host callback did not enter\n");
      _Exit(3);
    }
  }
  hrx_timeline_point_t previous = {0};
  for (unsigned step = 0; step < steps; ++step) {
    hrx_stream_t active = cross_queue && (step & 1) ? peer : stream;
    if (cross_queue && step)
      must(hrx_stream_wait_on(active, previous),
           "cross-queue producer dependency");
    const unsigned src = step % 2, dst = 1 - src;
    const hrx_buffer_ref_t bindings[] = {
        {b[src], G * sizeof(float), N * sizeof(float)},
        {b[2], G * sizeof(float), N * sizeof(float)},
        {b[dst], G * sizeof(float), N * sizeof(float)}};
    must(hrx_stream_dispatch(active, executable, ordinal, &config, &constants,
                             sizeof(constants), bindings, 3,
                             HRX_DISPATCH_FLAG_NONE),
         "dispatch");
    must(hrx_stream_flush(active), "producer flush");
    if (cross_queue)
      must(hrx_stream_get_timeline_position(active, &previous),
           "producer timeline");
    if (host_action && step == 1) {
      bool complete = true;
      must(hrx_stream_query(stream, &complete),
           "query consumer before releasing callback");
      if (complete ||
          atomic_load_explicit(&gate.finished, memory_order_acquire)) {
        fprintf(stderr, "FAIL consumer retired before host action release\n");
        _Exit(3);
      }
      puts("HOST_GATE consumer pending while A callback held; releasing A "
           "after B and C submissions");
      fflush(stdout);
      atomic_store_explicit(&gate.released, true, memory_order_release);
    }
  }
  must(hrx_stream_synchronize(stream), "retire queue 0 chain");
  if (peer)
    must(hrx_stream_synchronize(peer), "retire queue 1 chain");
  if (host_done) {
    must(hrx_semaphore_wait(host_done, 1, 5000000000ull),
         "host callback completion");
    if (!atomic_load_explicit(&gate.finished, memory_order_acquire))
      _Exit(3);
    hrx_semaphore_release(host_done);
  }
  for (unsigned buffer = 0; buffer < 3; ++buffer) {
    must(hrx_synchronous_d2h(device, b[buffer], 0, out, WORDS * sizeof(float)),
         "readback");
    for (unsigned i = 0; i < WORDS; ++i) {
      const float expected =
          (i < G || i >= G + N) ? guard
          : buffer == 2
              ? 1.0f
              : initial[i] + (float)(buffer == (steps % 2) ? steps : steps - 1);
      if (out[i] != expected) {
        fprintf(stderr, "FAIL buffer=%u word=%u got=%g expected=%g\n", buffer,
                i, (double)out[i], (double)expected);
        _Exit(3);
      }
    }
  }
  // All resources remain owned through confirmed completion and readback.
  for (unsigned i = 0; i < 3; ++i)
    hrx_buffer_release(b[i]);
  hrx_executable_release(executable);
  hrx_stream_release(stream);
  if (peer)
    hrx_stream_release(peer);
  must(hrx_gpu_shutdown(), "shutdown");
  alarm(0);
  free(initial);
  free(ones);
  free(out);
  printf("PASS steps=%u mode=%s queues=%u; 65533 exact outputs per ping-pong "
         "buffer; constant input and 64-byte guards unchanged; retired and "
         "shutdown\n",
         steps,
         cross_queue   ? "cross-queue"
         : host_action ? "host-action"
                       : "same-queue",
         queue_count);
  return 0;
}
