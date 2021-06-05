/*
 *  Copyright (c) 2021 Thakee Nathees
 *  Licensed under: MIT License
 */

#include "vm.h"

#include <math.h>
#include "core.h"
#include "utils.h"
#include "debug.h"

// Evaluvated to true if a runtime error set on the current fiber.
#define HAS_ERROR() (vm->fiber->error != NULL)

/*****************************************************************************/
/* VM PUBLIC API                                                             */
/*****************************************************************************/

// The default allocator that will be used to initialize the vm's configuration
// if the host doesn't provided any allocators for us.
static void* defaultRealloc(void* memory, size_t new_size, void* user_data);

PkConfiguration pkNewConfiguration() {
  PkConfiguration config;
  config.realloc_fn = defaultRealloc;

  config.error_fn = NULL;
  config.write_fn = NULL;

  config.load_script_fn = NULL;
  config.resolve_path_fn = NULL;
  config.user_data = NULL;

  return config;
}

PKVM* pkNewVM(PkConfiguration* config) {

  PkConfiguration default_config = pkNewConfiguration();

  if (config == NULL) config = &default_config;

  PKVM* vm = (PKVM*)config->realloc_fn(NULL, sizeof(PKVM), config->user_data);
  memset(vm, 0, sizeof(PKVM));

  vm->config = *config;
  vm->gray_list_count = 0;
  vm->gray_list_capacity = MIN_CAPACITY;
  vm->gray_list = (Object**)vm->config.realloc_fn(
                   NULL, sizeof(Object*) * vm->gray_list_capacity, NULL);
  vm->next_gc = INITIAL_GC_SIZE;
  vm->min_heap_size = MIN_HEAP_SIZE;
  vm->heap_fill_percent = HEAP_FILL_PERCENT;

  vm->scripts = newMap(vm);
  vm->core_libs = newMap(vm);
  vm->builtins_count = 0;

  initializeCore(vm);
  return vm;
}

void pkFreeVM(PKVM* vm) {

  Object* obj = vm->first;
  while (obj != NULL) {
    Object* next = obj->next;
    freeObject(vm, obj);
    obj = next;
  }

  vm->gray_list = (Object**)vm->config.realloc_fn(
    vm->gray_list, 0, vm->config.user_data);

  // Tell the host application that it forget to release all of it's handles
  // before freeing the VM.
  __ASSERT(vm->handles == NULL, "Not all handles were released.");

  DEALLOCATE(vm, vm);
}

void* pkGetUserData(const PKVM* vm) {
  return vm->config.user_data;
}

void pkSetUserData(PKVM* vm, void* user_data) {
  vm->config.user_data = user_data;
}

PkHandle* pkNewHandle(PKVM* vm, PkVar value) {
  return vmNewHandle(vm, *((Var*)value));
}

PkVar pkGetHandleValue(const PkHandle* handle) {
  return (PkVar)&handle->value;
}

void pkReleaseHandle(PKVM* vm, PkHandle* handle) {
  __ASSERT(handle != NULL, "Given handle was NULL.");

  // If the handle is the head of the vm's handle chain set it to the next one.
  if (handle == vm->handles) {
    vm->handles = handle->next;
  }

  // Remove the handle from the chain by connecting the both ends together.
  if (handle->next) handle->next->prev = handle->prev;
  if (handle->prev) handle->prev->next = handle->next;

  // Free the handle.
  DEALLOCATE(vm, handle);
}

PkHandle* vmNewHandle(PKVM* vm, Var value) {
  PkHandle* handle = (PkHandle*)ALLOCATE(vm, PkHandle);
  handle->value = value;
  handle->prev = NULL;
  handle->next = vm->handles;
  if (handle->next != NULL) handle->next->prev = handle;
  vm->handles = handle;
  return handle;
}


void* vmRealloc(PKVM* vm, void* memory, size_t old_size, size_t new_size) {

  // TODO: Debug trace allocations here.

  // Track the total allocated memory of the VM to trigger the GC.
  // if vmRealloc is called for freeing, the old_size would be 0 since 
  // deallocated bytes are traced by garbage collector.
  vm->bytes_allocated += new_size - old_size;

  if (new_size > 0 && vm->bytes_allocated > vm->next_gc) {
    vmCollectGarbage(vm);
  }

  return vm->config.realloc_fn(memory, new_size, vm->config.user_data);
}

void vmPushTempRef(PKVM* vm, Object* obj) {
  ASSERT(obj != NULL, "Cannot reference to NULL.");
  ASSERT(vm->temp_reference_count < MAX_TEMP_REFERENCE,
    "Too many temp references");
  vm->temp_reference[vm->temp_reference_count++] = obj;
}

void vmPopTempRef(PKVM* vm) {
  ASSERT(vm->temp_reference_count > 0,
         "Temporary reference is empty to pop.");
  vm->temp_reference_count--;
}

void vmCollectGarbage(PKVM* vm) {

  // Reset VM's bytes_allocated value and count it again so that we don't
  // required to know the size of each object that'll be freeing.
  vm->bytes_allocated = 0;

  // Mark the core libs and builtin functions.
  grayObject(vm, &vm->core_libs->_super);
  for (int i = 0; i < vm->builtins_count; i++) {
    grayObject(vm, &vm->builtins[i].fn->_super);
  }

  // Mark the scripts cache.
  grayObject(vm, &vm->scripts->_super);

  // Mark temp references.
  for (int i = 0; i < vm->temp_reference_count; i++) {
    grayObject(vm, vm->temp_reference[i]);
  }

  // Mark the handles.
  for (PkHandle* h = vm->handles; h != NULL; h = h->next) {
    grayValue(vm, h->value);
  }

  // Garbage collection triggered at the middle of a compilation.
  if (vm->compiler != NULL) {
    compilerMarkObjects(vm, vm->compiler);
  }

  if (vm->fiber != NULL) {
    grayObject(vm, &vm->fiber->_super);
  }

  blackenObjects(vm);

  // Now sweep all the un-marked objects in then link list and remove them
  // from the chain.

  // [ptr] is an Object* reference that should be equal to the next
  // non-garbage Object*.
  Object** ptr = &vm->first;
  while (*ptr != NULL) {

    // If the object the pointer points to wasn't marked it's unreachable.
    // Clean it. And update the pointer points to the next object.
    if (!(*ptr)->is_marked) {
      Object* garbage = *ptr;
      *ptr = garbage->next;
      freeObject(vm, garbage);

    } else {
      // Unmark the object for the next garbage collection.
      (*ptr)->is_marked = false;
      ptr = &(*ptr)->next;
    }
  }

  // Next GC heap size will be change depends on the byte we've left with now,
  // and the [heap_fill_percent].
  vm->next_gc = vm->bytes_allocated + (
    (vm->bytes_allocated * vm->heap_fill_percent) / 100);
  if (vm->next_gc < vm->min_heap_size) vm->next_gc = vm->min_heap_size;
}

/*****************************************************************************/
/* VM INTERNALS                                                              */
/*****************************************************************************/

// The default allocator that will be used to initialize the vm's configuration
// if the host doesn't provided any allocators for us.
static void* defaultRealloc(void* memory, size_t new_size, void* user_data) {
  if (new_size == 0) {
    free(memory);
    return NULL;
  }
  return realloc(memory, new_size);
}

static inline Script* getScript(PKVM* vm, String* path) {
  Var scr = mapGet(vm->scripts, VAR_OBJ(path));
  if (IS_UNDEF(scr)) return NULL;
  ASSERT(AS_OBJ(scr)->type == OBJ_SCRIPT, OOPS);
  return (Script*)AS_OBJ(scr);
}

// If failed to resolve it'll return false. Parameter [result] should be points
// to the string which is the path that has to be resolved and once it resolved
// the provided result's string's on_done() will be called and, it's string
// will be updated with the new resolved path string.
static inline bool resolveScriptPath(PKVM* vm, PkStringPtr* path_string) {
  if (vm->config.resolve_path_fn == NULL) return true;

  const char* path = path_string->string;
  PkStringPtr resolved;

  Fiber* fiber = vm->fiber;
  if (fiber == NULL || fiber->frame_count <= 0) {
    // fiber == NULL => vm haven't started yet and it's a root script.
    resolved = vm->config.resolve_path_fn(vm, NULL, path);
  } else {
    const Function* fn = fiber->frames[fiber->frame_count - 1].fn;
    resolved = vm->config.resolve_path_fn(vm, fn->owner->path->data, path);
  }

  // Done with the last string and update it with the new string.
  if (path_string->on_done != NULL) path_string->on_done(vm, *path_string);
  *path_string = resolved;

  return path_string->string != NULL;
}

// Import and return Script object as Var. If the script is imported and
// compiled here it'll set [is_new_script] to true oterwise (using the cached
// script) set to false.
static inline Var importScript(PKVM* vm, String* path_name) {

  // Check in the core libs.
  Script* scr = getCoreLib(vm, path_name);
  if (scr != NULL) return VAR_OBJ(scr);

  // Check in the scripts cache.
  Var entry = mapGet(vm->scripts, VAR_OBJ(path_name));
  if (!IS_UNDEF(entry)) {
    ASSERT(AS_OBJ(entry)->type == OBJ_SCRIPT, OOPS);
    return entry;
  }

  // Imported scripts were resolved at compile time.
  UNREACHABLE();

  return VAR_NULL;
}

static inline void growStack(PKVM* vm, int size) {
  Fiber* fiber = vm->fiber;
  ASSERT(fiber->stack_size <= size, OOPS);
  int new_size = utilPowerOf2Ceil(size);

  Var* old_rbp = fiber->stack; //< Old stack base pointer.
  fiber->stack = (Var*)vmRealloc(vm, fiber->stack,
                                 sizeof(Var) * fiber->stack_size,
                                 sizeof(Var) * new_size);
  fiber->stack_size = new_size;
  
  // If the old stack base pointer is the same as the current, that means the
  // stack hasn't been moved by the reallocation. In that case we're done.
  if (old_rbp == fiber->stack) return;

  // If we reached here that means the stack is moved by the reallocation and
  // we have to update all the pointers that pointing to the old stack slots.

  //
  //                                     '        '
  //             '        '              '        '
  //             '        '              |        | <new_rsp
  //    old_rsp> |        |              |        |
  //             |        |       .----> | value  | <new_ptr
  //             |        |       |      |        |
  //    old_ptr> | value  | ------'      |________| <new_rbp
  //             |        | ^            new stack
  //    old_rbp> |________| | height
  //             old stack
  //
  //            new_ptr = new_rbp      + height
  //                    = fiber->stack + ( old_ptr  - old_rbp )
#define MAP_PTR(old_ptr) (fiber->stack + ((old_ptr) - old_rbp))

  // Update the stack top pointer and the return pointer.
  fiber->sp = MAP_PTR(fiber->sp);
  fiber->ret = MAP_PTR(fiber->ret);

  // Update the stack base pointer of the call frames.
  for (int i = 0; i < fiber->frame_count; i++) {
    CallFrame* frame = fiber->frames + i;
    frame->rbp = MAP_PTR(frame->rbp);
  }
}

static inline void pushCallFrame(PKVM* vm, const Function* fn) {
    ASSERT(!fn->is_native, "Native function shouldn't use call frames.");

    /* Grow the stack frame if needed. */
    if (vm->fiber->frame_count + 1 > vm->fiber->frame_capacity) {
      int new_capacity = vm->fiber->frame_capacity << 1;
      vm->fiber->frames = (CallFrame*)vmRealloc(vm, vm->fiber->frames,
                             sizeof(CallFrame) * vm->fiber->frame_capacity,
                             sizeof(CallFrame) * new_capacity);
      vm->fiber->frame_capacity = new_capacity;
    }

    /* Grow the stack if needed. */
    int needed = fn->fn->stack_size + (int)(vm->fiber->sp - vm->fiber->stack);
    if (vm->fiber->stack_size <= needed) growStack(vm, needed);

    CallFrame* frame = vm->fiber->frames + vm->fiber->frame_count++;
    frame->rbp = vm->fiber->ret;
    frame->fn = fn;
    frame->ip = fn->fn->opcodes.data;
}

void pkSetRuntimeError(PKVM* vm, const char* message) {
  __ASSERT(vm->fiber != NULL, "This function can only be called at runtime.");
  vm->fiber->error = newString(vm, message);
}

void vmReportError(PKVM* vm) {
  ASSERT(HAS_ERROR(), "runtimeError() should be called after an error.");
  // TODO: pass the error to the caller of the fiber.

  // Print the Error message and stack trace.
  if (vm->config.error_fn == NULL) return;
  Fiber* fiber = vm->fiber;
  vm->config.error_fn(vm, PK_ERROR_RUNTIME, NULL, -1, fiber->error->data);
  for (int i = fiber->frame_count - 1; i >= 0; i--) {
    CallFrame* frame = &fiber->frames[i];
    const Function* fn = frame->fn;
    ASSERT(!fn->is_native, OOPS);
    int line = fn->fn->oplines.data[frame->ip - fn->fn->opcodes.data - 1];
    vm->config.error_fn(vm, PK_ERROR_STACKTRACE, fn->owner->path->data, line,
                        fn->name);
  }
}

// This function is responsible to call on_done function if it's done with the 
// provided string pointers.
PkInterpretResult pkInterpretSource(PKVM* vm, PkStringPtr source,
                                    PkStringPtr path) {
  String* path_name = newString(vm, path.string);
  if (path.on_done) path.on_done(vm, path);
  vmPushTempRef(vm, &path_name->_super); // path_name.

  // TODO: Should I clean the script if it already exists before compiling it?

  // Load a new script to the vm's scripts cache.
  Script* scr = getScript(vm, path_name);
  if (scr == NULL) {
    scr = newScript(vm, path_name);
    vmPushTempRef(vm, &scr->_super); // scr.
    mapSet(vm, vm->scripts, VAR_OBJ(path_name), VAR_OBJ(scr));
    vmPopTempRef(vm); // scr.
  }
  vmPopTempRef(vm); // path_name.

  // Compile the source.
  bool success = compile(vm, scr, source.string);
  if (source.on_done) source.on_done(vm, source);

  if (!success) return PK_RESULT_COMPILE_ERROR;

  // Set script initialized to true before the execution ends to prevent cyclic
  // inclusion cause a crash.
  scr->initialized = true;

  return vmRunFiber(vm, newFiber(vm, scr->body));
}

/******************************************************************************
 * RUNTIME                                                                    *
 *****************************************************************************/

PkInterpretResult vmRunFiber(PKVM* vm, Fiber* fiber) {

  // Set the fiber as the vm's current fiber (another root object) to prevent
  // it from garbage collection and get the reference from native functions.
  vm->fiber = fiber;

  ASSERT(fiber->state == FIBER_NEW, OOPS);
  fiber->state = FIBER_RUNNING;

  // The instruction pointer.
  // Note: sing 'uint8_t** ip' as reference to the instruction pointer in the
  // call frame seems a bit slower because of the dereferencing (~0.1 sec for
  // 100 million calls).
  register const uint8_t* ip;

  register Var* rbp;         //< Stack base pointer register.
  register CallFrame* frame; //< Current call frame.
  register Script* script;   //< Currently executing script.

#define PUSH(value)  (*vm->fiber->sp++ = (value))
#define POP()        (*(--vm->fiber->sp))
#define DROP()       (--vm->fiber->sp)
#define PEEK(off)    (*(vm->fiber->sp + (off)))
#define READ_BYTE()  (*ip++)
#define READ_SHORT() (ip+=2, (uint16_t)((ip[-2] << 8) | ip[-1]))

// Check if any runtime error exists and if so returns RESULT_RUNTIME_ERROR.
#define CHECK_ERROR()                 \
  do {                                \
    if (HAS_ERROR()) {                \
      UPDATE_FRAME();                 \
      vmReportError(vm);              \
      return PK_RESULT_RUNTIME_ERROR; \
    }                                 \
  } while (false)

// [err_msg] must be of type String.
#define RUNTIME_ERROR(err_msg)       \
  do {                               \
    vm->fiber->error = err_msg;      \
    UPDATE_FRAME();                  \
    vmReportError(vm);               \
    return PK_RESULT_RUNTIME_ERROR;  \
  } while (false)

// Load the last call frame to vm's execution variables to resume/run the
// function.
#define LOAD_FRAME()                                       \
  do {                                                     \
    frame = &vm->fiber->frames[vm->fiber->frame_count-1];  \
    ip = frame->ip;                                        \
    rbp = frame->rbp;                                      \
    script = frame->fn->owner;                             \
  } while (false)

// Update the frame's execution variables before pushing another call frame.
#define UPDATE_FRAME() frame->ip = ip

#ifdef OPCODE
  #error "OPCODE" should not be deifined here.
#endif

#if  DEBUG_DUMP_CALL_STACK
  #define DEBUG_CALL_STACK()        \
    do {                            \
      system("cls"); /* FIXME */    \
      dumpGlobalValues(vm);         \
      dumpStackFrame(vm);           \
    } while (false)
#else
  #define DEBUG_CALL_STACK() NO_OP
#endif

#define SWITCH() Opcode instruction; switch (instruction = (Opcode)READ_BYTE())
#define OPCODE(code) case OP_##code
#define DISPATCH()   goto L_vm_main_loop

  // TODO: remove the below push null and add it from the compiler.
  PUSH(VAR_NULL); // Return value of the script body.
  LOAD_FRAME();

  L_vm_main_loop:
  DEBUG_CALL_STACK();
  SWITCH() {

    OPCODE(PUSH_CONSTANT):
    {
      uint16_t index = READ_SHORT();
      ASSERT_INDEX(index, script->literals.count);
      PUSH(script->literals.data[index]);
      DISPATCH();
    }

    OPCODE(PUSH_NULL):
      PUSH(VAR_NULL);
      DISPATCH();

    OPCODE(PUSH_0):
      PUSH(VAR_NUM(0));
      DISPATCH();

    OPCODE(PUSH_TRUE):
      PUSH(VAR_TRUE);
      DISPATCH();

    OPCODE(PUSH_FALSE):
      PUSH(VAR_FALSE);
      DISPATCH();

    OPCODE(SWAP):
    {
      Var tmp = *(vm->fiber->sp - 1);
      *(vm->fiber->sp - 1) = *(vm->fiber->sp - 2);
      *(vm->fiber->sp - 2) = tmp;
      DISPATCH();
    }

    OPCODE(PUSH_LIST):
    {
      List* list = newList(vm, (uint32_t)READ_SHORT());
      PUSH(VAR_OBJ(list));
      DISPATCH();
    }

    OPCODE(PUSH_MAP):
    {
      Map* map = newMap(vm);
      PUSH(VAR_OBJ(map));
      DISPATCH();
    }

    OPCODE(LIST_APPEND):
    {
      Var elem = PEEK(-1); // Don't pop yet, we need the reference for gc.
      Var list = PEEK(-2);
      ASSERT(IS_OBJ_TYPE(list, OBJ_LIST), OOPS);
      varBufferWrite(&((List*)AS_OBJ(list))->elements, vm, elem);
      DROP(); // elem
      DISPATCH();
    }

    OPCODE(MAP_INSERT):
    {
      Var value = PEEK(-1); // Don't pop yet, we need the reference for gc.
      Var key = PEEK(-2);   // Don't pop yet, we need the reference for gc. 
      Var on = PEEK(-3);

      ASSERT(IS_OBJ_TYPE(on, OBJ_MAP), OOPS);

      if (IS_OBJ(key) && !isObjectHashable(AS_OBJ(key)->type)) {
        RUNTIME_ERROR(stringFormat(vm, "$ type is not hashable.",
                      varTypeName(key)));
      }
      mapSet(vm, (Map*)AS_OBJ(on), key, value);

      DROP(); // value
      DROP(); // key

      DISPATCH();
    }

    OPCODE(PUSH_LOCAL_0):
    OPCODE(PUSH_LOCAL_1):
    OPCODE(PUSH_LOCAL_2):
    OPCODE(PUSH_LOCAL_3):
    OPCODE(PUSH_LOCAL_4):
    OPCODE(PUSH_LOCAL_5):
    OPCODE(PUSH_LOCAL_6):
    OPCODE(PUSH_LOCAL_7):
    OPCODE(PUSH_LOCAL_8):
    {
      int index = (int)(instruction - OP_PUSH_LOCAL_0);
      PUSH(rbp[index + 1]); // +1: rbp[0] is return value.
      DISPATCH();
    }
    OPCODE(PUSH_LOCAL_N):
    {
      uint8_t index = READ_BYTE();
      PUSH(rbp[index + 1]);  // +1: rbp[0] is return value.
      DISPATCH();
    }

    OPCODE(STORE_LOCAL_0):
    OPCODE(STORE_LOCAL_1):
    OPCODE(STORE_LOCAL_2):
    OPCODE(STORE_LOCAL_3):
    OPCODE(STORE_LOCAL_4):
    OPCODE(STORE_LOCAL_5):
    OPCODE(STORE_LOCAL_6):
    OPCODE(STORE_LOCAL_7):
    OPCODE(STORE_LOCAL_8):
    {
      int index = (int)(instruction - OP_STORE_LOCAL_0);
      rbp[index + 1] = PEEK(-1);  // +1: rbp[0] is return value.
      DISPATCH();
    }
    OPCODE(STORE_LOCAL_N):
    {
      uint8_t index = READ_BYTE();
      rbp[index + 1] = PEEK(-1);  // +1: rbp[0] is return value.
      DISPATCH();
    }

    OPCODE(PUSH_GLOBAL):
    {
      uint8_t index = READ_BYTE();
      ASSERT(index < script->globals.count, OOPS);
      PUSH(script->globals.data[index]);
      DISPATCH();
    }

    OPCODE(STORE_GLOBAL):
    {
      uint8_t index = READ_BYTE();
      ASSERT(index < script->globals.count, OOPS);
      script->globals.data[index] = PEEK(-1);
      DISPATCH();
    }

    OPCODE(PUSH_FN):
    {
      uint8_t index = READ_BYTE();
      ASSERT(index < script->functions.count, OOPS);
      Function* fn = script->functions.data[index];
      PUSH(VAR_OBJ(fn));
      DISPATCH();
    }

    OPCODE(PUSH_BUILTIN_FN):
    {
      uint8_t index = READ_BYTE();
      ASSERT_INDEX(index, vm->builtins_count);
      Function* fn = vm->builtins[index].fn;
      PUSH(VAR_OBJ(fn));
      DISPATCH();
    }

    OPCODE(POP):
      DROP();
      DISPATCH();

    OPCODE(IMPORT):
    {
      String* name = script->names.data[READ_SHORT()];
      Var script = importScript(vm, name);

      // TODO: implement fiber bsed execution.
      //ASSERT(IS_OBJ_TYPE(script, OBJ_SCRIPT), OOPS);
      //Script* scr = (Script*)AS_OBJ(script);
      //if (!scr->initialized) vmRunScript(vm, scr);

      PUSH(script);
      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(CALL):
    {
      const uint8_t argc = READ_BYTE();

      // The call might change the vm->fiber so we need the reference to the
      // fiber that actually called the function.
      Fiber* call_fiber = vm->fiber;
      Var* callable = call_fiber->sp - argc - 1;

      if (IS_OBJ_TYPE(*callable, OBJ_FUNC)) {
        const Function* fn = (const Function*)AS_OBJ(*callable);

        // -1 argument means multiple number of args.
        if (fn->arity != -1 && fn->arity != argc) {
          char buff[STR_INT_BUFF_SIZE]; sprintf(buff, "%d", fn->arity);
          String* msg = stringFormat(vm, "Expected excatly $ argument(s).",
                                     buff);
          RUNTIME_ERROR(msg);
        }

        // Next call frame starts here. (including return value).
        call_fiber->ret = callable;
        *(call_fiber->ret) = VAR_NULL; //< Set the return value to null.

        if (fn->is_native) {

          if (fn->native == NULL) {
            RUNTIME_ERROR(stringFormat(vm,
              "Native function pointer of $ was NULL.", fn->name));
          }

          // Update the current frame's ip.
          UPDATE_FRAME();

          fn->native(vm); //< Call the native function.

          // Calling yield() will change vm->fiber to it's caller fiber, which
          // would be null if we're not running the function with a fiber.
          if (vm->fiber == NULL) return PK_RESULT_SUCCESS;

          // Load the top frame to vm's execution variables.
          if (vm->fiber != call_fiber) LOAD_FRAME();

          // Pop function arguments except for the return value.
          // Don't use 'vm->fiber' because calling fiber_new() and yield()
          // would change the fiber.
          call_fiber->sp = call_fiber->ret + 1;
          CHECK_ERROR();

        } else {
          UPDATE_FRAME(); //< Update the current frame's ip.
          pushCallFrame(vm, fn);
          LOAD_FRAME();  //< Load the top frame to vm's execution variables.
        }

      } else {
        RUNTIME_ERROR(stringFormat(vm, "$ $(@).", "Expected a function in "
        "call, instead got", varTypeName(*callable), toString(vm, *callable)));
      }

      DISPATCH();
    }

    OPCODE(ITER_TEST):
    {
      Var seq = PEEK(-3);

      // Primitive types are not iterable.
      if (!IS_OBJ(seq)) {
        if (IS_NULL(seq)) {
          RUNTIME_ERROR(newString(vm, "Null is not iterable."));
        } else if (IS_BOOL(seq)) {
          RUNTIME_ERROR(newString(vm, "Boolenan is not iterable."));
        } else if (IS_NUM(seq)) {
          RUNTIME_ERROR(newString(vm, "Number is not iterable."));
        } else {
          UNREACHABLE();
        }
      }

      DISPATCH();
    }

    OPCODE(ITER):
    {
      Var* value    = (vm->fiber->sp - 1);
      Var* iterator = (vm->fiber->sp - 2);
      Var seq       = PEEK(-3);
      uint16_t jump_offset = READ_SHORT();

    #define JUMP_ITER_EXIT() \
      do {                   \
        ip += jump_offset;   \
        DISPATCH();          \
      } while (false)

      ASSERT(IS_NUM(*iterator), OOPS);
      double it = AS_NUM(*iterator); //< Nth iteration.
      ASSERT(AS_NUM(*iterator) == (int32_t)trunc(it), OOPS);

      Object* obj = AS_OBJ(seq);
      switch (obj->type) {

        case OBJ_STRING: {
          uint32_t iter = (int32_t)trunc(it);

          // TODO: // Need to consider utf8.
          String* str = ((String*)obj);
          if (iter >= str->length) JUMP_ITER_EXIT();

          //TODO: vm's char (and reusable) strings.
          *value = VAR_OBJ(newStringLength(vm, str->data + iter, 1));
          *iterator = VAR_NUM((double)iter + 1);

        } DISPATCH();

        case OBJ_LIST: {
          uint32_t iter = (int32_t)trunc(it);
          VarBuffer* elems = &((List*)obj)->elements;
          if (iter >= elems->count) JUMP_ITER_EXIT();
          *value = elems->data[iter];
          *iterator = VAR_NUM((double)iter + 1);
          
        } DISPATCH();

        case OBJ_MAP: {
          uint32_t iter = (int32_t)trunc(it);

          Map* map = (Map*)obj;
          if (map->entries == NULL) JUMP_ITER_EXIT();
          MapEntry* e = map->entries + iter;
          for (; iter < map->capacity; iter++, e++) {
            if (!IS_UNDEF(e->key)) JUMP_ITER_EXIT();
          }
          if (iter >= map->capacity) JUMP_ITER_EXIT();

          *value = map->entries[iter].key;
          *iterator = VAR_NUM((double)iter + 1);
          
        } DISPATCH();

        case OBJ_RANGE: {
          double from = ((Range*)obj)->from;
          double to = ((Range*)obj)->to;
          if (from == to) JUMP_ITER_EXIT();

          double current;
          if (from <= to) { //< Straight range.
            current = from + it;
          } else {          //< Reversed range.
            current = from - it;
          }
          if (current == to) JUMP_ITER_EXIT();
          *value = VAR_NUM(current);
          *iterator = VAR_NUM(it + 1);
          
        } DISPATCH();

        case OBJ_SCRIPT:
        case OBJ_FUNC:
        case OBJ_FIBER:
        case OBJ_USER:
          TODO; break;
        default:
          UNREACHABLE();
      }

      DISPATCH();
    }

    OPCODE(JUMP):
    {
      uint16_t offset = READ_SHORT();
      ip += offset;
      DISPATCH();
    }

    OPCODE(LOOP):
    {
      uint16_t offset = READ_SHORT();
      ip -= offset;
      DISPATCH();
    }

    OPCODE(JUMP_IF):
    {
      Var cond = POP();
      uint16_t offset = READ_SHORT();
      if (toBool(cond)) {
        ip += offset;
      }
      DISPATCH();
    }

    OPCODE(JUMP_IF_NOT):
    {
      Var cond = POP();
      uint16_t offset = READ_SHORT();
      if (!toBool(cond)) {
        ip += offset;
      }
      DISPATCH();
    }

    OPCODE(RETURN):
    {

      // Set the return value.
      Var ret_value = POP();

      // Pop the last frame, and if no more call frames, we're done with the
      // current fiber.
      if (--vm->fiber->frame_count == 0) {
        vm->fiber->state = FIBER_DONE;

        // TODO:
        //vm->fiber->sp = vm->fiber->stack;.

        if (vm->fiber->caller == NULL) {
          return PK_RESULT_SUCCESS;

        } else {
          Fiber* caller = vm->fiber->caller;
          ASSERT(caller->state == FIBER_RUNNING, OOPS);

          vm->fiber->caller = NULL;
          vm->fiber = caller;
          *caller->ret = ret_value;
        }

      } else {
        *rbp = ret_value;
        // Pop the params (locals should have popped at this point) and update
        // stack pointer.
        vm->fiber->sp = rbp + 1; // +1: rbp is returned value.
      }

      LOAD_FRAME();
      DISPATCH();
    }

    OPCODE(GET_ATTRIB):
    {
      Var on = PEEK(-1); // Don't pop yet, we need the reference for gc. 
      String* name = script->names.data[READ_SHORT()];
      Var value = varGetAttrib(vm, on, name);
      DROP(); // on
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(GET_ATTRIB_KEEP):
    {
      Var on = PEEK(-1);
      String* name = script->names.data[READ_SHORT()];
      PUSH(varGetAttrib(vm, on, name));
      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(SET_ATTRIB):
    {
      Var value = PEEK(-1); // Don't pop yet, we need the reference for gc. 
      Var on = PEEK(-2);    // Don't pop yet, we need the reference for gc. 
      String* name = script->names.data[READ_SHORT()];
      varSetAttrib(vm, on, name, value);

      DROP(); // value
      DROP(); // on
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(GET_SUBSCRIPT):
    {
      Var key = PEEK(-1); // Don't pop yet, we need the reference for gc.
      Var on = PEEK(-2);  // Don't pop yet, we need the reference for gc.
      Var value = varGetSubscript(vm, on, key);
      DROP(); // key
      DROP(); // on
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(GET_SUBSCRIPT_KEEP):
    {
      Var key = PEEK(-1);
      Var on = PEEK(-2);
      PUSH(varGetSubscript(vm, on, key));
      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(SET_SUBSCRIPT):
    {
      Var value = PEEK(-1); // Don't pop yet, we need the reference for gc.
      Var key = PEEK(-2);   // Don't pop yet, we need the reference for gc.
      Var on = PEEK(-3);    // Don't pop yet, we need the reference for gc.
      varsetSubscript(vm, on, key, value);
      DROP(); // value
      DROP(); // key
      DROP(); // on
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(NEGATIVE):
    {
      Var num = POP();
      if (!IS_NUM(num)) {
        RUNTIME_ERROR(newString(vm, "Cannot negate a non numeric value."));
      }
      PUSH(VAR_NUM(-AS_NUM(num)));
      DISPATCH();
    }

    OPCODE(NOT):
    {
      Var val = POP();
      PUSH(VAR_BOOL(!toBool(val)));
      DISPATCH();
    }

    OPCODE(BIT_NOT):
      TODO;

    // Do not ever use PUSH(binaryOp(vm, POP(), POP()));
    // Function parameters are not evaluated in a defined order in C.

    OPCODE(ADD):
    {
      // Don't pop yet, we need the reference for gc.
      Var r = PEEK(-1), l = PEEK(-2);
      Var value = varAdd(vm, l, r);
      DROP(); DROP(); // r, l
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(SUBTRACT):
    {
      // Don't pop yet, we need the reference for gc.
      Var r = PEEK(-1), l = PEEK(-2);
      Var value = varSubtract(vm, l, r);
      DROP(); DROP(); // r, l
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(MULTIPLY):
    {
      // Don't pop yet, we need the reference for gc.
      Var r = PEEK(-1), l = PEEK(-2);
      Var value = varMultiply(vm, l, r);
      DROP(); DROP(); // r, l
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(DIVIDE):
    {
      // Don't pop yet, we need the reference for gc.
      Var r = PEEK(-1), l = PEEK(-2);
      Var value = varDivide(vm, l, r);
      DROP(); DROP(); // r, l
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(MOD):
    {
      // Don't pop yet, we need the reference for gc.
      Var r = PEEK(-1), l = PEEK(-2);
      Var value = varModulo(vm, l, r);
      DROP(); DROP(); // r, l
      PUSH(value);

      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(BIT_AND):
    OPCODE(BIT_OR):
    OPCODE(BIT_XOR):
    OPCODE(BIT_LSHIFT):
    OPCODE(BIT_RSHIFT):
      TODO;

    OPCODE(EQEQ):
    {
      Var r = POP(), l = POP();
      PUSH(VAR_BOOL(isValuesEqual(l, r)));
      DISPATCH();
    }

    OPCODE(NOTEQ):
    {
      Var r = POP(), l = POP();
      PUSH(VAR_BOOL(!isValuesEqual(l, r)));
      DISPATCH();
    }

    OPCODE(LT):
    {
      Var r = POP(), l = POP();
      PUSH(VAR_BOOL(varLesser(l, r)));
      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(LTEQ):
    {
      Var r = POP(), l = POP();
      bool lteq = varLesser(l, r);
      CHECK_ERROR();

      if (!lteq) {
        lteq = isValuesEqual(l, r);
        CHECK_ERROR();
      }

      PUSH(VAR_BOOL(lteq));
      DISPATCH();
    }

    OPCODE(GT):
    {
      Var r = POP(), l = POP();
      PUSH(VAR_BOOL(varGreater(l, r)));
      CHECK_ERROR();
      DISPATCH();
    }

    OPCODE(GTEQ):
    {
      Var r = POP(), l = POP();
      bool gteq = varGreater(l, r);
      CHECK_ERROR();

      if (!gteq) {
        gteq = isValuesEqual(l, r);
        CHECK_ERROR();
      }

      PUSH(VAR_BOOL(gteq));
      DISPATCH();
    }

    OPCODE(RANGE):
    {
      Var to = PEEK(-1);   // Don't pop yet, we need the reference for gc.
      Var from = PEEK(-2); // Don't pop yet, we need the reference for gc.
      if (!IS_NUM(from) || !IS_NUM(to)) {
        RUNTIME_ERROR(newString(vm, "Range arguments must be number."));
      }
      DROP(); // to
      DROP(); // from
      PUSH(VAR_OBJ(newRange(vm, AS_NUM(from), AS_NUM(to))));
      DISPATCH();
    }

    OPCODE(IN):
      // TODO: Implement bool varContaines(vm, on, value);
      TODO;

    OPCODE(END):
      TODO;
      break;

    default:
      UNREACHABLE();

  }

  return PK_RESULT_SUCCESS;
}
