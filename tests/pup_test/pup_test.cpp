#include "conv-rdma.h"
#include "converse.h"
#include "pup.h"
#include <stdio.h>
#include <string.h>

// Message structure for Converse
struct Message {
  CmiMessageHeader header;
};

// Test class that demonstrates PUP serialization
class TestData {
public:
  int intValue;
  double doubleValue;
  char stringValue[256];
  int intArray[10];
  int arraySize;
  CmiNcpyBuffer ncpyBuffer;

  TestData() : intValue(0), doubleValue(0.0), arraySize(0) {
    stringValue[0] = '\0';
    memset(intArray, 0, sizeof(intArray));
  }

  TestData(int i, double d, const char *s, int *arr, int arrSize)
      : intValue(i), doubleValue(d), arraySize(arrSize) {
    strncpy(stringValue, s, sizeof(stringValue) - 1);
    stringValue[sizeof(stringValue) - 1] = '\0';

    for (int j = 0; j < arrSize && j < 10; j++) {
      intArray[j] = arr[j];
    }

    // Initialize ncpyBuffer with some test data
    char *testData = (char *)malloc(100);
    for (int j = 0; j < 100; j++) {
      testData[j] = 'A' + (j % 26);
    }
    ncpyBuffer = CmiNcpyBuffer(testData, 100);
  }

  void pup(PUP::er &p) {
    p(intValue);
    p(doubleValue);
    p(stringValue, 256); // Serialize as char array
    p(arraySize);        // Serialize array size first
    p(intArray, arraySize);
    // Note: CmiNcpyBuffer has its own pup method, so we can use it directly
    ncpyBuffer.pup(p);
  }

  void print() const {
    printf("TestData: int=%d, double=%.2f, string='%s', array=[", intValue,
           doubleValue, stringValue);
    for (int i = 0; i < arraySize; i++) {
      printf("%d", intArray[i]);
      if (i < arraySize - 1)
        printf(", ");
    }
    printf("], ncpyBuffer.ptr=%p, ncpyBuffer.cnt=%zu\n", ncpyBuffer.ptr,
           ncpyBuffer.cnt);
  }
};

// Simple test class without PUPable inheritance
class SimpleTestClass {
public:
  int value;
  char name[256];

  SimpleTestClass() : value(0) { name[0] = '\0'; }

  SimpleTestClass(int v, const char *n) : value(v) {
    strncpy(name, n, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
  }

  void pup(PUP::er &p) {
    p(value);
    p(name, 256); // Serialize as char array
  }

  void print() const {
    printf("SimpleTestClass: value=%d, name='%s'\n", value, name);
  }
};

CpvDeclare(int, testHandlerId);
CpvDeclare(int, exitHandlerId);

void test_handler(void *vmsg) {
  printf("=== PUP Test Handler Called ===\n");

  // Test 1: Basic PUP serialization with TestData
  printf("\n--- Test 1: Basic PUP Serialization ---\n");
  int testArray[] = {1, 2, 3, 4, 5};
  TestData original(42, 3.14159, "Hello PUP!", testArray, 5);
  printf("Original data:\n");
  original.print();

  // Size the data
  PUP::sizer sizer;
  sizer | original;
  size_t dataSize = sizer.size();
  printf("Serialized size: %zu bytes\n", dataSize);

  // Pack the data
  char *buffer = (char *)malloc(dataSize);
  PUP::toMem packer(buffer);
  packer | original;
  printf("Packed %zu bytes\n", packer.size());

  // Unpack the data
  TestData restored;
  PUP::fromMem unpacker(buffer);
  unpacker | restored;
  printf("Restored data:\n");
  restored.print();

  // Verify data integrity
  bool dataMatch = (original.intValue == restored.intValue) &&
                   (original.doubleValue == restored.doubleValue) &&
                   (strcmp(original.stringValue, restored.stringValue) == 0) &&
                   (original.arraySize == restored.arraySize);

  // Check array contents
  for (int i = 0; i < original.arraySize && dataMatch; i++) {
    if (original.intArray[i] != restored.intArray[i]) {
      dataMatch = false;
    }
  }

  printf("Data integrity check: %s\n", dataMatch ? "PASSED" : "FAILED");

  // Test 2: Simple class serialization
  printf("\n--- Test 2: Simple Class Serialization ---\n");
  SimpleTestClass originalSimple(100, "TestObject");
  printf("Original Simple:\n");
  originalSimple.print();

  // Size the simple object
  PUP::sizer sizer2;
  sizer2 | originalSimple;
  size_t simpleSize = sizer2.size();
  printf("Simple serialized size: %zu bytes\n", simpleSize);

  // Pack the simple object
  char *buffer2 = (char *)malloc(simpleSize);
  PUP::toMem packer2(buffer2);
  packer2 | originalSimple;
  printf("Packed Simple %zu bytes\n", packer2.size());

  // Unpack the simple object
  SimpleTestClass restoredSimple;
  PUP::fromMem unpacker2(buffer2);
  unpacker2 | restoredSimple;
  printf("Restored Simple:\n");
  restoredSimple.print();

  // Verify simple object integrity
  bool simpleMatch = (originalSimple.value == restoredSimple.value) &&
                     (strcmp(originalSimple.name, restoredSimple.name) == 0);
  printf("Simple object integrity check: %s\n",
         simpleMatch ? "PASSED" : "FAILED");

  // Test 3: Network byte order serialization
  printf("\n--- Test 3: Network Byte Order Serialization ---\n");
  int networkArray[] = {0xDEAD, 0xBEEF, 0xCAFE};
  TestData networkTest(0x12345678, 2.71828, "Network Test", networkArray, 3);
  printf("Original network test data:\n");
  networkTest.print();

  // Size for network serialization
  PUP::sizer networkSizer;
  networkSizer | networkTest;
  size_t networkSize = networkSizer.size();
  printf("Network serialized size: %zu bytes\n", networkSize);

  // Pack to network byte order
  char *networkBuffer = (char *)malloc(networkSize);
  PUP::toMem networkPacker(networkBuffer);
  networkPacker | networkTest;
  printf("Packed to network format %zu bytes\n", networkPacker.size());

  // Unpack from network byte order
  TestData networkRestored;
  PUP::fromMem networkUnpacker(networkBuffer);
  networkUnpacker | networkRestored;
  printf("Restored from network format:\n");
  networkRestored.print();

  // Cleanup
  free(buffer);
  free(buffer2);
  free(networkBuffer);

  printf("\n=== All PUP Tests Completed ===\n");

  // Send exit message
  Message *exitMsg = new Message;
  exitMsg->header.handlerId = CpvAccess(exitHandlerId);
  exitMsg->header.messageSize = sizeof(Message);
  CmiSyncSendAndFree(0, exitMsg->header.messageSize, exitMsg);
}

void exit_handler(void *vmsg) {
  printf("PUP test completed successfully!\n");
  CsdExitScheduler();
}

CmiStartFn mymain(int argc, char **argv) {
  printf("Starting PUP Test Program on PE %d\n", CmiMyRank());

  // Register handlers
  CpvInitialize(int, testHandlerId);
  CpvAccess(testHandlerId) = CmiRegisterHandler(test_handler);

  CpvInitialize(int, exitHandlerId);
  CpvAccess(exitHandlerId) = CmiRegisterHandler(exit_handler);

  if (CmiMyRank() == 0) {
    // Create and send test message
    Message *msg = new Message;
    msg->header.handlerId = CpvAccess(testHandlerId);
    msg->header.messageSize = sizeof(Message);
    CmiSyncSendAndFree(0, msg->header.messageSize, msg);
  }

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
