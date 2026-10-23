// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dbus/object_proxy.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/mock_bus.h"
#include "dbus/object_path.h"
#include "dbus/test_service.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::Return;
using ::testing::SetArgPointee;

namespace dbus {
namespace {

class ObjectProxyTest : public testing::Test {
 protected:
  ObjectProxyTest() {}

  void SetUp() override {
    Bus::Options bus_options;
    bus_options.bus_type = Bus::SESSION;
    bus_options.connection_type = Bus::PRIVATE;
    bus_ = new Bus(std::move(bus_options));
  }

  void TearDown() override { bus_->ShutdownAndBlock(); }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};

  scoped_refptr<Bus> bus_;
};

// Runs ObjectProxy on a MockBus, so a test can control what the bus returns
// without a real D-Bus connection.
class ObjectProxyMockBusTest : public testing::Test {
 protected:
  ObjectProxyMockBusTest() {}

  void SetUp() override {
    Bus::Options bus_options;
    bus_options.bus_type = Bus::SESSION;
    bus_options.connection_type = Bus::PRIVATE;
    mock_bus_ = new MockBus(std::move(bus_options));

    // Run both the origin and the D-Bus side of ObjectProxy on the test's
    // main thread.
    scoped_refptr<base::SequencedTaskRunner> task_runner =
        base::SequencedTaskRunner::GetCurrentDefault();
    EXPECT_CALL(*mock_bus_, GetOriginTaskRunner())
        .WillRepeatedly(Return(task_runner.get()));
    EXPECT_CALL(*mock_bus_, GetDBusTaskRunner())
        .WillRepeatedly(Return(task_runner.get()));
    EXPECT_CALL(*mock_bus_, AssertOnOriginThread()).WillRepeatedly(Return());
    EXPECT_CALL(*mock_bus_, AssertOnDBusThread()).WillRepeatedly(Return());
    EXPECT_CALL(*mock_bus_, IsConnected()).WillRepeatedly(Return(false));

    object_proxy_ = new ObjectProxy(mock_bus_.get(), "org.chromium.TestService",
                                    ObjectPath("/org/chromium/TestObject"),
                                    ObjectProxy::DEFAULT_OPTIONS);
  }

  void TearDown() override { object_proxy_->Detach(); }

  base::test::TaskEnvironment task_environment_;

  scoped_refptr<MockBus> mock_bus_;
  scoped_refptr<ObjectProxy> object_proxy_;
};

// Used as a ResponseOrErrorCallback. Records whether each pointer was set.
void OnResponseOrError(int* num_calls,
                       bool* has_response,
                       bool* has_error_response,
                       Response* response,
                       ErrorResponse* error_response) {
  *has_response = response != nullptr;
  *has_error_response = error_response != nullptr;
  (*num_calls)++;
}

// Used as a WaitForServiceToBeAvailableCallback.
void OnServiceIsAvailable(bool* dest_service_is_available,
                          int* num_calls,
                          bool src_service_is_available) {
  *dest_service_is_available = src_service_is_available;
  (*num_calls)++;
}

// Used as a callback for TestService::RequestOwnership().
void OnOwnershipRequestDone(bool success) {
  ASSERT_TRUE(success);
}

// Used as a callback for TestService::ReleaseOwnership().
void OnOwnershipReleased() {}

TEST_F(ObjectProxyTest, WaitForServiceToBeAvailableRunOnce) {
  TestService::Options options;
  TestService test_service(options);
  ObjectProxy* object_proxy = bus_->GetObjectProxy(
      test_service.service_name(), ObjectPath("/org/chromium/TestObject"));

  // The callback is not yet called because the service is not available.
  int num_calls = 0;
  bool service_is_available = false;
  object_proxy->WaitForServiceToBeAvailable(
      base::BindOnce(&OnServiceIsAvailable, &service_is_available, &num_calls));
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(0, num_calls);

  // Start the service. The callback should be called asynchronously.
  ASSERT_TRUE(test_service.StartService());
  test_service.WaitUntilServiceIsStarted();
  ASSERT_TRUE(test_service.has_ownership());
  num_calls = 0;
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, num_calls);
  EXPECT_TRUE(service_is_available);

  // Release the service's ownership of its name. The callback should not be
  // invoked again.
  test_service.ReleaseOwnership(base::BindOnce(&OnOwnershipReleased));
  num_calls = 0;
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(0, num_calls);

  // Take ownership of the name and check that the callback is not called.
  test_service.RequestOwnership(base::BindOnce(&OnOwnershipRequestDone));
  num_calls = 0;
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(0, num_calls);
}

TEST_F(ObjectProxyTest, WaitForServiceToBeAvailableAlreadyRunning) {
  TestService::Options options;
  TestService test_service(options);
  ObjectProxy* object_proxy = bus_->GetObjectProxy(
      test_service.service_name(), ObjectPath("/org/chromium/TestObject"));

  ASSERT_TRUE(test_service.StartService());
  test_service.WaitUntilServiceIsStarted();
  ASSERT_TRUE(test_service.has_ownership());

  // Since the service is already running, the callback should be invoked
  // immediately (but asynchronously, rather than the callback being invoked
  // directly within WaitForServiceToBeAvailable()).
  int num_calls = 0;
  bool service_is_available = false;
  object_proxy->WaitForServiceToBeAvailable(
      base::BindOnce(&OnServiceIsAvailable, &service_is_available, &num_calls));
  EXPECT_EQ(0, num_calls);

  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, num_calls);
  EXPECT_TRUE(service_is_available);
}

TEST_F(ObjectProxyTest, WaitForServiceToBeAvailableMultipleCallbacks) {
  TestService::Options options;
  TestService test_service(options);
  ObjectProxy* object_proxy = bus_->GetObjectProxy(
      test_service.service_name(), ObjectPath("/org/chromium/TestObject"));

  // Register two callbacks.
  int num_calls_1 = 0, num_calls_2 = 0;
  bool service_is_available_1 = false, service_is_available_2 = false;
  object_proxy->WaitForServiceToBeAvailable(base::BindOnce(
      &OnServiceIsAvailable, &service_is_available_1, &num_calls_1));
  object_proxy->WaitForServiceToBeAvailable(base::BindOnce(
      &OnServiceIsAvailable, &service_is_available_2, &num_calls_2));
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(0, num_calls_1);
  EXPECT_EQ(0, num_calls_2);

  // Start the service and confirm that both callbacks are invoked.
  ASSERT_TRUE(test_service.StartService());
  test_service.WaitUntilServiceIsStarted();
  ASSERT_TRUE(test_service.has_ownership());
  num_calls_1 = 0;
  num_calls_2 = 0;
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, num_calls_1);
  EXPECT_EQ(1, num_calls_2);
  EXPECT_TRUE(service_is_available_1);
  EXPECT_TRUE(service_is_available_2);
}

// dbus_connection_send_with_reply() succeeds without creating a pending call
// when the connection is already disconnected. The method call must then fail
// through the callback instead of passing the null pending call to libdbus,
// which aborts the process.
TEST_F(ObjectProxyMockBusTest, NullPendingCallRunsCallbackWithNull) {
  EXPECT_CALL(*mock_bus_, Connect()).WillOnce(Return(true));
  EXPECT_CALL(*mock_bus_, SetUpAsyncOperations()).WillOnce(Return(true));
  EXPECT_CALL(*mock_bus_, SendWithReply(_, _, _))
      .WillOnce(SetArgPointee<1>(static_cast<DBusPendingCall*>(nullptr)));

  MethodCall method_call("org.chromium.TestInterface", "Echo");
  int num_calls = 0;
  bool has_response = true;
  bool has_error_response = true;
  object_proxy_->CallMethodWithErrorResponse(
      &method_call, ObjectProxy::TIMEOUT_USE_DEFAULT,
      base::BindOnce(&OnResponseOrError, &num_calls, &has_response,
                     &has_error_response));

  // The callback is invoked asynchronously, not from within
  // CallMethodWithErrorResponse().
  EXPECT_EQ(0, num_calls);

  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, num_calls);
  EXPECT_FALSE(has_response);
  EXPECT_FALSE(has_error_response);

  // The callback is not invoked again.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, num_calls);
}

}  // namespace
}  // namespace dbus
