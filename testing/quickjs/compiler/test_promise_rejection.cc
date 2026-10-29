// Copyright 2024 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "quickjs/include/quickjs-libc.h"
#include "quickjs/include/quickjs.h"
#ifdef __cplusplus
}
#endif
#include "gc/trace-gc.h"
#include "quickjs/include/quickjs-inner.h"
namespace promise_test {

class PromiseRejectionTest : public ::testing::Test {
 protected:
  PromiseRejectionTest() = default;
  ~PromiseRejectionTest() override = default;

  void SetUp() override {
    rt_ = LEPUS_NewRuntime();
    ctx_ = LEPUS_NewContext(rt_);
  }

  void TearDown() override {
    LEPUS_FreeContext(ctx_);
    LEPUS_FreeRuntime(rt_);
  }

  LEPUSContext* ctx_;
  LEPUSRuntime* rt_;
};

static void js_print(LEPUSContext* ctx, LEPUSValueConst this_val, int argc,
                     LEPUSValueConst* argv, std::string& result) {
  int i;
  const char* str;
  for (i = 0; i < argc; i++) {
    if (i != 0) result += ' ';
    str = LEPUS_ToCString(ctx, argv[i]);
    result += str;
    if (!ctx->rt->gc_enable) LEPUS_FreeCString(ctx, str);
  }
  result += "\n";
}

static std::string js_get_exception_string(LEPUSContext* ctx) {
  std::string result = "";
  LEPUSValue exception_val, val;
  const char* stack;
  uint8_t is_error;

  exception_val = LEPUS_GetException(ctx);
  HandleScope func_scope(ctx, &exception_val, HANDLE_TYPE_LEPUS_VALUE);
  is_error = LEPUS_IsError(ctx, exception_val);
  if (!is_error) result += "Throw: ";

  js_print(ctx, LEPUS_NULL, 1, (LEPUSValueConst*)&exception_val, result);
  if (is_error) {
    val = LEPUS_GetPropertyStr(ctx, exception_val, "stack");
    if (!LEPUS_IsUndefined(val)) {
      stack = LEPUS_ToCString(ctx, val);
      result += stack;
      // result += "\n";
      if (!ctx->rt->gc_enable) LEPUS_FreeCString(ctx, stack);
    }
    if (!ctx->rt->gc_enable) LEPUS_FreeValue(ctx, val);
  }
  if (!ctx->rt->gc_enable) LEPUS_FreeValue(ctx, exception_val);
  return result;
}

static std::string js_dump_unhandled_rejection(LEPUSContext* ctx) {
  std::string result = "";
  int count = 0;
  while (LEPUS_MoveUnhandledRejectionToException(ctx)) {
    result += js_get_exception_string(ctx);
    count++;
  }
  // if (count == 0) return result;
  return result;
}

static bool js_run(LEPUSContext* ctx, const char* filename, LEPUSValue& ret) {
  uint8_t* buf;
  int eval_flags;
  size_t buf_len;
  buf = lepus_load_file(ctx, &buf_len, filename);
  if (!buf) {
    ret = LEPUS_UNDEFINED;
    return false;
  }
  eval_flags = LEPUS_EVAL_TYPE_GLOBAL;
  ret = LEPUS_Eval(ctx, (const char*)buf, buf_len, filename, eval_flags);
  free(buf);
  return true;
}

TEST_F(PromiseRejectionTest, AsyncStackTraceTest) {
  const char* filename = TEST_CASE_DIR "unit_test/async_stack_trace_test.js";
  LEPUSValue val;
  bool res = js_run(ctx_, filename, val);
  if (res) {
    std::string result = "";
    if (LEPUS_IsException(val)) {
      result += js_get_exception_string(ctx_);
    }
    if (!ctx_->rt->gc_enable) LEPUS_FreeValue(ctx_, val);

    lepus_std_loop(ctx_);
    result += js_dump_unhandled_rejection(ctx_);
    ASSERT_TRUE(result == "");
  } else {
    if (!ctx_->rt->gc_enable) LEPUS_FreeValue(ctx_, val);
    ASSERT_TRUE(false);
  }
}

TEST_F(PromiseRejectionTest, RejectionReasonObjectTest) {
  const char* filename = TEST_CASE_DIR "unit_test/rejection_reason_object.js";
  LEPUSValue val;
  bool res = js_run(ctx_, filename, val);
  if (res) {
    std::string result = "";
    if (LEPUS_IsException(val)) {
      result += js_get_exception_string(ctx_);
    }
    if (!ctx_->rt->gc_enable) LEPUS_FreeValue(ctx_, val);

    lepus_std_loop(ctx_);
    result += js_dump_unhandled_rejection(ctx_);
    ASSERT_TRUE(result == "");
  } else {
    if (!ctx_->rt->gc_enable) LEPUS_FreeValue(ctx_, val);
    ASSERT_TRUE(false);
  }
}

// Own an API return in RC mode and root its stable slot in tracing mode.
// Construct slots before a call that fills them so later allocations and
// explicit collections cannot collect a returned value between assertions.
class RejectionOwnedValue final {
 public:
  explicit RejectionOwnedValue(LEPUSContext* ctx)
      : ctx_(ctx), tracing_(LEPUS_IsGCMode(ctx)) {
    if (tracing_) {
      root_ =
          std::make_unique<HandleScope>(ctx_, &value_, HANDLE_TYPE_LEPUS_VALUE);
    }
  }

  ~RejectionOwnedValue() { Reset(); }
  RejectionOwnedValue(const RejectionOwnedValue&) = delete;
  RejectionOwnedValue& operator=(const RejectionOwnedValue&) = delete;

  LEPUSValue& value() { return value_; }
  LEPUSValue* address() { return &value_; }

  void Reset() {
    const LEPUSValue previous = value_;
    value_ = LEPUS_UNDEFINED;
    if (!tracing_) LEPUS_FreeValue(ctx_, previous);
  }

 private:
  LEPUSContext* ctx_;
  bool tracing_;
  LEPUSValue value_ = LEPUS_UNDEFINED;
  std::unique_ptr<HandleScope> root_;
};

// Keep the existing stack-trace fixture and its cleanup behavior unchanged.
class RejectionOwnershipTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rt_ = LEPUS_NewRuntime();
    ASSERT_NE(rt_, nullptr);
    ctx_ = LEPUS_NewContext(rt_);
    ASSERT_NE(ctx_, nullptr);
    tracing_ = LEPUS_IsGCMode(ctx_);
    RecordProperty("rejection_memory_mode",
                   tracing_ ? "tracing" : "reference_counting");
  }

  void TearDown() override { DestroyRuntime(); }

  void DestroyRuntime() {
    // Every local value/root must have left scope before this is called.
    if (ctx_) {
      LEPUSContext* context = ctx_;
      ctx_ = nullptr;
      LEPUS_FreeContext(context);
    }
    if (rt_) {
      LEPUSRuntime* runtime = rt_;
      rt_ = nullptr;
      LEPUS_FreeRuntime(runtime);
    }
  }

  std::size_t PendingUnhandled() {
    const list_head* head = &rt_->unhandled_rejections;
    const list_head* entry = head->next;
    std::size_t count = 0;
    // These cases create at most three entries. Bound a corrupt cycle so the
    // queue setup/assertion cannot hang indefinitely.
    while (entry != head && count <= 3) {
      if (!entry) {
        ADD_FAILURE() << "rejection queue contains a null link";
        return 4;
      }
      ++count;
      entry = entry->next;
    }
    EXPECT_LE(count, 3u);
    return count;
  }

  void QueueOnly(const char* script, std::size_t expected) {
    RejectionOwnedValue result(ctx_);
    result.value() =
        LEPUS_Eval(ctx_, script, std::strlen(script), "rejection-ownership.js",
                   LEPUS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(LEPUS_IsException(result.value()));
    ASSERT_TRUE(LEPUS_IsUndefined(result.value()));
    ASSERT_EQ(PendingUnhandled(), expected);
    ASSERT_TRUE(list_empty(&rt_->handled_rejections));
    ASSERT_TRUE(LEPUS_IsNull(rt_->current_exception));
  }

  void ExpectErrorMessage(LEPUSValueConst error, const char* expected) {
    ASSERT_TRUE(LEPUS_IsError(ctx_, error));
    RejectionOwnedValue message(ctx_);
    message.value() = LEPUS_GetPropertyStr(ctx_, error, "message");
    ASSERT_FALSE(LEPUS_IsException(message.value()));
    ASSERT_TRUE(LEPUS_IsString(message.value()));
    const char* text = LEPUS_ToCString(ctx_, message.value());
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text, expected);
    if (!tracing_) LEPUS_FreeCString(ctx_, text);
  }

  void CollectReturnedValuesIfTracing() {
    if (tracing_) LEPUS_RunGC(rt_);
  }

  LEPUSRuntime* rt_ = nullptr;
  LEPUSContext* ctx_ = nullptr;
  bool tracing_ = false;
};

TEST_F(RejectionOwnershipTest, QueueOnlySingleRuntimeTeardown) {
  ASSERT_NO_FATAL_FAILURE(
      QueueOnly("Promise.reject(new Error('single')); void 0;", 1));
  // No global, C++ output, or remaining evaluation result owns the Promise.
  DestroyRuntime();
  EXPECT_EQ(ctx_, nullptr);
  EXPECT_EQ(rt_, nullptr);
  RecordProperty("explicit_runtime_teardown", "completed");
}

TEST_F(RejectionOwnershipTest, QueueOnlyThreeRuntimeTeardown) {
  ASSERT_NO_FATAL_FAILURE(
      QueueOnly("Promise.reject(new Error('first'));"
                "Promise.reject(new Error('second'));"
                "Promise.reject(new Error('third')); void 0;",
                3));
  DestroyRuntime();
  EXPECT_EQ(ctx_, nullptr);
  EXPECT_EQ(rt_, nullptr);
  RecordProperty("explicit_runtime_teardown", "completed");
}

TEST_F(RejectionOwnershipTest, MoveTransfersQueueOnlyErrorsInFifoOrder) {
  ASSERT_NO_FATAL_FAILURE(
      QueueOnly("Promise.reject(new Error('first'));"
                "Promise.reject(new Error('second'));"
                "Promise.reject(new Error('third')); void 0;",
                3));
  const std::array<const char*, 3> messages{{"first", "second", "third"}};
  for (std::size_t index = 0; index < messages.size(); ++index) {
    SCOPED_TRACE(index);
    RejectionOwnedValue error(ctx_);
    ASSERT_EQ(LEPUS_MoveUnhandledRejectionToException(ctx_), 1);
    error.value() = LEPUS_GetException(ctx_);
    ASSERT_TRUE(LEPUS_IsNull(rt_->current_exception));
    ASSERT_EQ(PendingUnhandled(), messages.size() - index - 1);
    CollectReturnedValuesIfTracing();
    ASSERT_NO_FATAL_FAILURE(ExpectErrorMessage(error.value(), messages[index]));
  }
  EXPECT_EQ(LEPUS_MoveUnhandledRejectionToException(ctx_), 0);
  EXPECT_EQ(PendingUnhandled(), 0u);
  EXPECT_TRUE(list_empty(&rt_->handled_rejections));
}

class RejectionOwnershipOutputTest
    : public RejectionOwnershipTest,
      public ::testing::WithParamInterface<unsigned> {};

TEST_P(RejectionOwnershipOutputTest, NullableOutputsOwnIndependentValues) {
  const unsigned mask = GetParam();  // Promise=1, reason=2, reporting error=4.
  RecordProperty("output_mask", static_cast<int>(mask));
  ASSERT_NO_FATAL_FAILURE(
      QueueOnly("Promise.reject(new Error('owned-reason')); void 0;", 1));

  RejectionOwnedValue promise(ctx_);
  RejectionOwnedValue reason(ctx_);
  RejectionOwnedValue error(ctx_);
  promise.value() = LEPUS_NewInt32(ctx_, 101);
  reason.value() = LEPUS_NewInt32(ctx_, 102);
  error.value() = LEPUS_NewInt32(ctx_, 103);
  ASSERT_EQ(
      LEPUS_TakeUnhandledRejection(ctx_, mask & 1 ? promise.address() : nullptr,
                                   mask & 2 ? reason.address() : nullptr,
                                   mask & 4 ? error.address() : nullptr),
      1);
  ASSERT_EQ(PendingUnhandled(), 0u);
  ASSERT_TRUE(list_empty(&rt_->handled_rejections));
  ASSERT_TRUE(LEPUS_IsNull(rt_->current_exception));

  CollectReturnedValuesIfTracing();
  if (mask & 1) {
    ASSERT_TRUE(LEPUS_IsObject(promise.value()));
    EXPECT_EQ(LEPUS_GetClassID(ctx_, promise.value()), JS_CLASS_PROMISE);
  }
  if ((mask & 6) == 6) {
    // Error reasons share object identity with the reporting error, while
    // both output slots must still own independent API references.
    EXPECT_TRUE(LEPUS_SameValue(ctx_, reason.value(), error.value()));
  }
  // The returned Promise itself retains its result. Drop it before checking
  // the other outputs so it cannot hide a missing owned reason/error return.
  promise.Reset();
  CollectReturnedValuesIfTracing();
  if (mask & 2) {
    ASSERT_NO_FATAL_FAILURE(ExpectErrorMessage(reason.value(), "owned-reason"));
  }
  reason.Reset();
  CollectReturnedValuesIfTracing();
  if (mask & 4) {
    ASSERT_NO_FATAL_FAILURE(ExpectErrorMessage(error.value(), "owned-reason"));
  }
  error.Reset();

  // An empty take overwrites each requested sentinel with undefined. Output
  // slots are distinct; this test does not create a new pointer-alias policy.
  promise.value() = LEPUS_NewInt32(ctx_, 101);
  reason.value() = LEPUS_NewInt32(ctx_, 102);
  error.value() = LEPUS_NewInt32(ctx_, 103);
  EXPECT_EQ(
      LEPUS_TakeUnhandledRejection(ctx_, mask & 1 ? promise.address() : nullptr,
                                   mask & 2 ? reason.address() : nullptr,
                                   mask & 4 ? error.address() : nullptr),
      0);
  if (mask & 1) EXPECT_TRUE(LEPUS_IsUndefined(promise.value()));
  if (mask & 2) EXPECT_TRUE(LEPUS_IsUndefined(reason.value()));
  if (mask & 4) EXPECT_TRUE(LEPUS_IsUndefined(error.value()));
  EXPECT_EQ(PendingUnhandled(), 0u);
}

INSTANTIATE_TEST_SUITE_P(OutputMasks, RejectionOwnershipOutputTest,
                         ::testing::Values(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u),
                         [](const ::testing::TestParamInfo<unsigned>& info) {
                           return "Mask" + std::to_string(info.param);
                         });

class HandledRejectionOwnershipTest : public RejectionOwnershipTest {
 protected:
  std::size_t PendingHandled() {
    const list_head* head = &rt_->handled_rejections;
    const list_head* entry = head->next;
    std::size_t count = 0;
    while (entry != head && count <= 3) {
      if (!entry) {
        ADD_FAILURE() << "handled rejection queue contains a null link";
        return 4;
      }
      ++count;
      entry = entry->next;
    }
    EXPECT_LE(count, 3u);
    return count;
  }

  void ExpectHandledHeadMessage(const char* message) {
    ASSERT_FALSE(list_empty(&rt_->handled_rejections));
    const auto* entry = list_entry(rt_->handled_rejections.next,
                                   JSUnhandledRejectionEntry, link);
    // The queue owns this borrowed reason during inspection. No value/extra
    // reference escapes this helper to protect a later TakeHandled call.
    ASSERT_NO_FATAL_FAILURE(ExpectErrorMessage(entry->reason, message));
  }

  void AttachTwoResolvingHandlers(LEPUSValueConst promise,
                                  std::size_t expected_handled) {
    RejectionOwnedValue method(ctx_);
    RejectionOwnedValue handler(ctx_);
    RejectionOwnedValue child(ctx_);
    method.value() = LEPUS_GetPropertyStr(ctx_, promise, "catch");
    ASSERT_TRUE(LEPUS_IsFunction(ctx_, method.value()));
    const char* script = "(function() {})";
    handler.value() =
        LEPUS_Eval(ctx_, script, std::strlen(script), "rejection-handler.js",
                   LEPUS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(LEPUS_IsException(handler.value()));
    ASSERT_TRUE(LEPUS_IsFunction(ctx_, handler.value()));
    LEPUSValueConst args[] = {handler.value()};
    for (unsigned index = 0; index < 2; ++index) {
      child.Reset();
      child.value() = LEPUS_Call(ctx_, method.value(), promise, 1, args);
      ASSERT_FALSE(LEPUS_IsException(child.value()));
      ASSERT_TRUE(LEPUS_IsObject(child.value()));
      EXPECT_EQ(LEPUS_GetClassID(ctx_, child.value()), JS_CLASS_PROMISE);
      ASSERT_EQ(PendingHandled(), expected_handled);
    }
  }

  void DrainHandlerJobs() {
    unsigned executed = 0;
    while (LEPUS_IsJobPending(rt_)) {
      // At most six resolving handlers are installed by these fixtures.
      // This is a test-loop guard, not an engine execution-budget change.
      ASSERT_LT(executed, 16u);
      LEPUSContext* job_context = nullptr;
      ASSERT_EQ(LEPUS_ExecutePendingJob(rt_, &job_context), 1);
      ASSERT_EQ(job_context, ctx_);
      ++executed;
    }
    ASSERT_GT(executed, 0u);
    ASSERT_TRUE(LEPUS_IsNull(rt_->current_exception));
    RecordProperty("drained_handler_jobs", static_cast<int>(executed));
  }

  void QueueReportedHandled(std::size_t count) {
    ASSERT_TRUE(count == 1 || count == 3);
    const char* script = count == 1
                             ? "Promise.reject(new Error('first')); void 0;"
                             : "Promise.reject(new Error('first'));"
                               "Promise.reject(new Error('second'));"
                               "Promise.reject(new Error('third')); void 0;";
    ASSERT_NO_FATAL_FAILURE(QueueOnly(script, count));
    for (std::size_t index = 0; index < count; ++index) {
      RejectionOwnedValue promise(ctx_);
      // Reporting keeps exactly the returned Promise alive until its catch
      // handler is attached; no reason/error output or global is retained.
      ASSERT_EQ(LEPUS_TakeUnhandledRejection(ctx_, promise.address(), nullptr,
                                             nullptr),
                1);
      ASSERT_TRUE(LEPUS_IsObject(promise.value()));
      ASSERT_EQ(PendingUnhandled(), count - index - 1);
      ASSERT_NO_FATAL_FAILURE(
          AttachTwoResolvingHandlers(promise.value(), index + 1));
      promise.Reset();
    }
    ASSERT_NO_FATAL_FAILURE(DrainHandlerJobs());
    ASSERT_FALSE(LEPUS_IsJobPending(rt_));
    ASSERT_EQ(PendingUnhandled(), 0u);
    ASSERT_EQ(PendingHandled(), count);
    // All local outputs/handler children are gone and jobs have drained.
    // The handled queue is the remaining fixture owner of each Promise.
  }
};

TEST_F(HandledRejectionOwnershipTest, EarlyHandlersRetirePendingNotification) {
  {
    RejectionOwnedValue promise(ctx_);
    const char* script = "Promise.reject(new Error('early'))";
    promise.value() = LEPUS_Eval(ctx_, script, std::strlen(script),
                                 "early-rejection.js", LEPUS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(LEPUS_IsException(promise.value()));
    ASSERT_TRUE(LEPUS_IsObject(promise.value()));
    ASSERT_EQ(PendingUnhandled(), 1u);
    ASSERT_EQ(PendingHandled(), 0u);
    ASSERT_NO_FATAL_FAILURE(AttachTwoResolvingHandlers(promise.value(), 0));
    ASSERT_EQ(PendingUnhandled(), 0u);
    promise.Reset();
  }
  ASSERT_NO_FATAL_FAILURE(DrainHandlerJobs());
  CollectReturnedValuesIfTracing();
  EXPECT_EQ(PendingUnhandled(), 0u);
  EXPECT_EQ(PendingHandled(), 0u);
  EXPECT_EQ(LEPUS_TakeUnhandledRejection(ctx_, nullptr, nullptr, nullptr), 0);
  EXPECT_EQ(LEPUS_TakeHandledRejection(ctx_, nullptr, nullptr), 0);
}

TEST_F(HandledRejectionOwnershipTest, QueueOnlySingleHandledRuntimeTeardown) {
  ASSERT_NO_FATAL_FAILURE(QueueReportedHandled(1));
  CollectReturnedValuesIfTracing();
  ASSERT_EQ(PendingHandled(), 1u);
  DestroyRuntime();
  EXPECT_EQ(ctx_, nullptr);
  EXPECT_EQ(rt_, nullptr);
  RecordProperty("explicit_handled_runtime_teardown", "completed");
}

TEST_F(HandledRejectionOwnershipTest, QueueOnlyThreeHandledRuntimeTeardown) {
  ASSERT_NO_FATAL_FAILURE(QueueReportedHandled(3));
  CollectReturnedValuesIfTracing();
  ASSERT_EQ(PendingHandled(), 3u);
  DestroyRuntime();
  EXPECT_EQ(ctx_, nullptr);
  EXPECT_EQ(rt_, nullptr);
  RecordProperty("explicit_handled_runtime_teardown", "completed");
}

class HandledRejectionOwnershipOutputTest
    : public HandledRejectionOwnershipTest,
      public ::testing::WithParamInterface<unsigned> {};

TEST_P(HandledRejectionOwnershipOutputTest,
       ReportedNotificationsAreFifoAndNullable) {
  const unsigned mask = GetParam();  // Promise=1, reason=2.
  RecordProperty("handled_output_mask", static_cast<int>(mask));
  ASSERT_NO_FATAL_FAILURE(QueueReportedHandled(3));
  const std::array<const char*, 3> messages{{"first", "second", "third"}};
  for (std::size_t index = 0; index < messages.size(); ++index) {
    SCOPED_TRACE(index);
    ASSERT_NO_FATAL_FAILURE(ExpectHandledHeadMessage(messages[index]));
    RejectionOwnedValue promise(ctx_);
    RejectionOwnedValue reason(ctx_);
    promise.value() = LEPUS_NewInt32(ctx_, 201);
    reason.value() = LEPUS_NewInt32(ctx_, 202);
    ASSERT_EQ(
        LEPUS_TakeHandledRejection(ctx_, mask & 1 ? promise.address() : nullptr,
                                   mask & 2 ? reason.address() : nullptr),
        1);
    ASSERT_EQ(PendingHandled(), messages.size() - index - 1);
    ASSERT_EQ(PendingUnhandled(), 0u);
    CollectReturnedValuesIfTracing();
    if (mask & 1) {
      ASSERT_TRUE(LEPUS_IsObject(promise.value()));
      EXPECT_EQ(LEPUS_GetClassID(ctx_, promise.value()), JS_CLASS_PROMISE);
    }
    // Drop the Promise's own result reference before checking that a returned
    // reason has independent ownership after the handled entry is retired.
    promise.Reset();
    CollectReturnedValuesIfTracing();
    if (mask & 2) {
      ASSERT_NO_FATAL_FAILURE(
          ExpectErrorMessage(reason.value(), messages[index]));
    }
  }
  RejectionOwnedValue promise(ctx_);
  RejectionOwnedValue reason(ctx_);
  promise.value() = LEPUS_NewInt32(ctx_, 201);
  reason.value() = LEPUS_NewInt32(ctx_, 202);
  EXPECT_EQ(
      LEPUS_TakeHandledRejection(ctx_, mask & 1 ? promise.address() : nullptr,
                                 mask & 2 ? reason.address() : nullptr),
      0);
  if (mask & 1) EXPECT_TRUE(LEPUS_IsUndefined(promise.value()));
  if (mask & 2) EXPECT_TRUE(LEPUS_IsUndefined(reason.value()));
  EXPECT_EQ(PendingHandled(), 0u);
  EXPECT_EQ(PendingUnhandled(), 0u);
  EXPECT_FALSE(LEPUS_IsJobPending(rt_));
}

INSTANTIATE_TEST_SUITE_P(HandledOutputMasks,
                         HandledRejectionOwnershipOutputTest,
                         ::testing::Values(0u, 1u, 2u, 3u),
                         [](const ::testing::TestParamInfo<unsigned>& info) {
                           return "Mask" + std::to_string(info.param);
                         });

// RC FinalizationRegistry callbacks run synchronously when the last owned
// reason is released. They must observe the retired notification as absent,
// even when they consume the next notification from the same queue.
class RejectionReentryTest : public HandledRejectionOwnershipTest {
 protected:
  void SetUp() override {
    rt_ = LEPUS_NewRuntime();
    ASSERT_NE(rt_, nullptr);
    LEPUS_SetRuntimeInfo(rt_, "Lynx_JS_RC");
    ctx_ = LEPUS_NewContext(rt_);
    ASSERT_NE(ctx_, nullptr);
    tracing_ = LEPUS_IsGCMode(ctx_);
    ASSERT_FALSE(tracing_);
    RecordProperty("rejection_memory_mode", "reference_counting");
    LEPUS_SetContextOpaque(ctx_, this);
    RejectionOwnedValue global(ctx_);
    global.value() = LEPUS_GetGlobalObject(ctx_);
    ASSERT_EQ(
        LEPUS_SetPropertyStr(
            ctx_, global.value(), "consumeNextRejection",
            LEPUS_NewCFunction(ctx_, ConsumeNext, "consumeNextRejection", 0)),
        1);
  }

  static LEPUSValue ConsumeNext(LEPUSContext* ctx, LEPUSValueConst, int,
                                LEPUSValueConst*) {
    auto* self =
        static_cast<RejectionReentryTest*>(LEPUS_GetContextOpaque(ctx));
    ++self->callbacks_;
    self->observed_pending_ =
        self->handled_ ? self->PendingHandled() : self->PendingUnhandled();
    self->taken_ =
        self->handled_
            ? LEPUS_TakeHandledRejection(ctx, nullptr, nullptr)
            : LEPUS_TakeUnhandledRejection(ctx, nullptr, nullptr, nullptr);
    return LEPUS_UNDEFINED;
  }

  void QueueRegisteredReasons() {
    ASSERT_NO_FATAL_FAILURE(QueueOnly(
        "globalThis.rejectionRegistry = new FinalizationRegistry("
        "consumeNextRejection);"
        "(() => { const reason = {tag: 'first'};"
        "rejectionRegistry.register(reason, 0); Promise.reject(reason); })();"
        "Promise.reject({tag: 'second'}); void 0;",
        2));
    ASSERT_EQ(callbacks_, 0u);
  }

  void ExpectConsumedNext() {
    EXPECT_EQ(callbacks_, 1u);
    EXPECT_EQ(observed_pending_, 1u);
    EXPECT_EQ(taken_, 1);
    EXPECT_EQ(PendingUnhandled(), 0u);
    EXPECT_EQ(PendingHandled(), 0u);
    RecordProperty("reentrant_cleanup_callbacks", static_cast<int>(callbacks_));
  }

  bool handled_ = false;
  unsigned callbacks_ = 0;
  std::size_t observed_pending_ = 0;
  int taken_ = -1;
};

TEST_F(RejectionReentryTest, TakeDetachesBeforeReasonFinalization) {
  ASSERT_NO_FATAL_FAILURE(QueueRegisteredReasons());
  ASSERT_EQ(LEPUS_TakeUnhandledRejection(ctx_, nullptr, nullptr, nullptr), 1);
  ASSERT_NO_FATAL_FAILURE(ExpectConsumedNext());
  EXPECT_TRUE(LEPUS_IsNull(rt_->current_exception));
}

TEST_F(RejectionReentryTest, MoveDetachesBeforeReasonFinalization) {
  ASSERT_NO_FATAL_FAILURE(QueueRegisteredReasons());
  ASSERT_EQ(LEPUS_MoveUnhandledRejectionToException(ctx_), 1);
  ASSERT_NO_FATAL_FAILURE(ExpectConsumedNext());
  // The nested take clears the outer reporting exception by the existing API
  // contract; no output reference is borrowed across that reentrant call.
  EXPECT_TRUE(LEPUS_IsNull(rt_->current_exception));
}

TEST_F(RejectionReentryTest, HandledTakeDetachesBeforeReasonFinalization) {
  ASSERT_NO_FATAL_FAILURE(QueueRegisteredReasons());
  for (unsigned index = 0; index < 2; ++index) {
    RejectionOwnedValue promise(ctx_);
    ASSERT_EQ(
        LEPUS_TakeUnhandledRejection(ctx_, promise.address(), nullptr, nullptr),
        1);
    ASSERT_NO_FATAL_FAILURE(
        AttachTwoResolvingHandlers(promise.value(), index + 1));
  }
  ASSERT_NO_FATAL_FAILURE(DrainHandlerJobs());
  ASSERT_EQ(PendingUnhandled(), 0u);
  ASSERT_EQ(PendingHandled(), 2u);
  ASSERT_EQ(callbacks_, 0u);
  handled_ = true;
  ASSERT_EQ(LEPUS_TakeHandledRejection(ctx_, nullptr, nullptr), 1);
  ASSERT_NO_FATAL_FAILURE(ExpectConsumedNext());
  EXPECT_TRUE(LEPUS_IsNull(rt_->current_exception));
}

}  // namespace promise_test
