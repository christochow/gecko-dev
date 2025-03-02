/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Array.h"  // JS::GetArrayLength, JS::IsArrayObject
#include "js/Object.h"  // JS::GetClass
#include "jsapi-tests/tests.h"

static bool constructHook(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);

  // Check that arguments were passed properly from JS_New.

  JS::RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    JS_ReportErrorASCII(cx, "test failed, could not construct object");
    return false;
  }
  if (strcmp(JS::GetClass(obj)->name, "Object") != 0) {
    JS_ReportErrorASCII(cx, "test failed, wrong class for 'this'");
    return false;
  }
  if (args.length() != 3) {
    JS_ReportErrorASCII(cx, "test failed, argc == %d", args.length());
    return false;
  }
  if (!args[0].isInt32() || args[2].toInt32() != 2) {
    JS_ReportErrorASCII(cx, "test failed, wrong value in args[2]");
    return false;
  }
  if (!args.isConstructing()) {
    JS_ReportErrorASCII(cx, "test failed, not constructing");
    return false;
  }

  // Perform a side-effect to indicate that this hook was actually called.
  JS::RootedValue value(cx, args[0]);
  JS::RootedObject callee(cx, &args.callee());
  if (!JS_SetElement(cx, callee, 0, value)) {
    return false;
  }

  args.rval().setObject(*obj);

  // trash the argv, perversely
  args[0].setUndefined();
  args[1].setUndefined();
  args[2].setUndefined();

  return true;
}

BEGIN_TEST(testNewObject_1) {
  static const size_t N = 1000;
  JS::RootedValueVector argv(cx);
  CHECK(argv.resize(N));

  JS::RootedValue v(cx);
  EVAL("Array", &v);
  JS::RootedObject Array(cx, v.toObjectOrNull());

  bool isArray;

  // With no arguments.
  JS::RootedObject obj(cx, JS_New(cx, Array, JS::HandleValueArray::empty()));
  CHECK(obj);
  JS::RootedValue rt(cx, JS::ObjectValue(*obj));
  CHECK(JS::IsArrayObject(cx, obj, &isArray));
  CHECK(isArray);
  uint32_t len;
  CHECK(JS::GetArrayLength(cx, obj, &len));
  CHECK_EQUAL(len, 0u);

  // With one argument.
  argv[0].setInt32(4);
  obj = JS_New(cx, Array, JS::HandleValueArray::subarray(argv, 0, 1));
  CHECK(obj);
  rt = JS::ObjectValue(*obj);
  CHECK(JS::IsArrayObject(cx, obj, &isArray));
  CHECK(isArray);
  CHECK(JS::GetArrayLength(cx, obj, &len));
  CHECK_EQUAL(len, 4u);

  // With N arguments.
  for (size_t i = 0; i < N; i++) {
    argv[i].setInt32(i);
  }
  obj = JS_New(cx, Array, JS::HandleValueArray::subarray(argv, 0, N));
  CHECK(obj);
  rt = JS::ObjectValue(*obj);
  CHECK(JS::IsArrayObject(cx, obj, &isArray));
  CHECK(isArray);
  CHECK(JS::GetArrayLength(cx, obj, &len));
  CHECK_EQUAL(len, N);
  CHECK(JS_GetElement(cx, obj, N - 1, &v));
  CHECK(v.isInt32(N - 1));

  // With JSClass.construct.
  static const JSClassOps clsOps = {
      nullptr,        // addProperty
      nullptr,        // delProperty
      nullptr,        // enumerate
      nullptr,        // newEnumerate
      nullptr,        // resolve
      nullptr,        // mayResolve
      nullptr,        // finalize
      nullptr,        // call
      nullptr,        // hasInstance
      constructHook,  // construct
      nullptr,        // trace
  };
  static const JSClass cls = {"testNewObject_1", 0, &clsOps};
  JS::RootedObject ctor(cx, JS_NewObject(cx, &cls));
  CHECK(ctor);
  JS::RootedValue rt2(cx, JS::ObjectValue(*ctor));
  obj = JS_New(cx, ctor, JS::HandleValueArray::subarray(argv, 0, 3));
  CHECK(obj);
  CHECK(JS_GetElement(cx, ctor, 0, &v));
  CHECK(v.isInt32(0));

  return true;
}
END_TEST(testNewObject_1)

BEGIN_TEST(testNewObject_IsMapObject) {
  // Test IsMapObject and IsSetObject

  JS::RootedValue vMap(cx);
  EVAL("Map", &vMap);
  JS::RootedObject Map(cx, vMap.toObjectOrNull());

  bool isMap = false;
  bool isSet = false;
  JS::RootedObject mapObj(cx, JS_New(cx, Map, JS::HandleValueArray::empty()));
  CHECK(mapObj);
  CHECK(JS::IsMapObject(cx, mapObj, &isMap));
  CHECK(isMap);
  CHECK(JS::IsSetObject(cx, mapObj, &isSet));
  CHECK(!isSet);

  JS::RootedValue vSet(cx);
  EVAL("Set", &vSet);
  JS::RootedObject Set(cx, vSet.toObjectOrNull());

  JS::RootedObject setObj(cx, JS_New(cx, Set, JS::HandleValueArray::empty()));
  CHECK(setObj);
  CHECK(JS::IsMapObject(cx, setObj, &isMap));
  CHECK(!isMap);
  CHECK(JS::IsSetObject(cx, setObj, &isSet));
  CHECK(isSet);

  return true;
}
END_TEST(testNewObject_IsMapObject)

static const JSClassOps Base_classOps = {
    nullptr,  // addProperty
    nullptr,  // delProperty
    nullptr,  // enumerate
    nullptr,  // newEnumerate
    nullptr,  // resolve
    nullptr,  // mayResolve
    nullptr,  // finalize
    nullptr,  // call
    nullptr,  // hasInstance
    nullptr,  // construct
    nullptr,  // trace
};

static const JSClass Base_class = {"Base",
                                   0,  // flags
                                   &Base_classOps};

BEGIN_TEST(testNewObject_Subclassing) {
  JSObject* proto =
      JS_InitClass(cx, global, nullptr, &Base_class, Base_constructor, 0,
                   nullptr, nullptr, nullptr, nullptr);
  if (!proto) {
    return false;
  }

  // Calling Base without `new` should fail with a TypeError.
  JS::RootedValue expectedError(cx);
  EVAL("TypeError", &expectedError);
  JS::RootedValue actualError(cx);
  EVAL(
      "try {\n"
      "  Base();\n"
      "} catch (e) {\n"
      "  e.constructor;\n"
      "}\n",
      &actualError);
  CHECK_SAME(actualError, expectedError);

  // Check prototype chains when a JS class extends a base class that's
  // implemented in C++ using JS_NewObjectForConstructor.
  EXEC(
      "class MyClass extends Base {\n"
      "  ok() { return true; }\n"
      "}\n"
      "let myObj = new MyClass();\n");

  JS::RootedValue result(cx);
  EVAL("myObj.ok()", &result);
  CHECK_SAME(result, JS::TrueValue());

  EVAL("myObj.__proto__ === MyClass.prototype", &result);
  CHECK_SAME(result, JS::TrueValue());
  EVAL("myObj.__proto__.__proto__ === Base.prototype", &result);
  CHECK_SAME(result, JS::TrueValue());

  EVAL("myObj", &result);
  CHECK_EQUAL(JS::GetClass(&result.toObject()), &Base_class);

  return true;
}

static bool Base_constructor(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  JS::RootedObject obj(cx, JS_NewObjectForConstructor(cx, &Base_class, args));
  if (!obj) {
    return false;
  }
  args.rval().setObject(*obj);
  return true;
}

END_TEST(testNewObject_Subclassing)
