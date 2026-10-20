# Language Design

## Specs

- Forced `copy` and move keyword BEFORE structs
- All rvalues move implicility while lvalues are explicit about move/copy
- Casting is exactly explicit if the type is concrete
- `readonly` is a member modifier and only means this cannot be changed from OUTSIDE
- `readonly` applies to member variables only
- `readonly` applies to primitives only
- `readonly` is valid only for public for tidiness it's error for private fields
- imports must be toplevel
- only structs with copy constructor can be copied
- every error must terminate somehow
- moves invalidate any further use of something
- optionals can be refined
- null exists only as a value for optionals
- member functions have `this: T*` automatically not needed to be entered. static functions do not have `this: T*` generics get statics for each instantiation
- static functions are only for members
- move is bitwise memcpy with invalidating of the field (aliases are ignored? maybe depends on the difficulty)
- compile time (`comptime`) expressions are `T(Type).(size|is_copyable|has_destructor|is_primitive)`
- T& is not valid generic metatype
- `try abort` is a terminator for all* errors, upon errors it cleans up leftover memory and does exit(1)
- `abort ($ERR1$,$ERR2,...$)` can ignore a specific errorcode at the end of a try catch block or if no () block it ignores all errors
- Polymorphism for later
- No `operator =` exists any compound x= operator calls the underlying `operator x`
- A `const` variable cannot be downgraded to non-const in any way or by calling a function that takes non-const for that parameter
- A `const` function is valid only as a member function and must not mutate `this`
- `T=V*` => `*T = V = V&`
- A `move` marked function paramter must be strictly moved
- `alloc`, `realloc`, `free`, `memcpy`, `init`, `destroy`, `drop`, and `slice` are intrinsics
- `init(p, move v)` doesn't call destructor on pointer unlike `*p = v`
- `destroy(v*)` calls deconstructor manually
- `drop(move v)` is empty intrinsic
- Per _project_ hura.toml(or build.hura) file
- References
  - Cannot be moved
  - Cannot be mutated
  - Cannot be reseated (only assigns internally not pointed mutation)
  - Can only interact with through `const` functions and variables
  - `&x` -> `T*` and `T*` ->? `T&` (one way)

### Casts

- `cast` Conversion of compatible types: int->float, T*->void*, in->im (m>n)
- `trunc` Conversion of decimal values by shortening: float->int, in->im or fn->fm (n>m)
- `bitcast` Convert representation of something (size checked)
- `ptrcast` Unchecked T*->U* cast

### Internal

```cpp
import hura:fmt

private extern struct slice<T> { // extern : no default value for members
    // T[] hidden to user
    private ptr: T*;
    public readonly length: usize;

    private slice() {}

    public operator [](usize i) -> T& {
        return ptr[i];
    }
}

public extern slice<T>(ptr: T*, size: usize) -> T[];

public struct string {
    private ptr: u8*? = null;
    public readonly length: usize = 0;

    public string() {}

    public string(from: u8[]) {
        ptr = try abort alloc::<u8>(from.length);
        memcpy(ptr, from.ptr, from.length);
        length = from.length;
    }

    public copy string(copy_from: string &){
        ptr = try abort alloc::<u8>(copy_from.length);
        memcpy(ptr, copy_from.ptr, copy_from.length);
        length = copy_from.length;
    }

    public ~string() {
        free(ptr);
    }
}
```

### Errors

There are many ways to handle errors

- Try without catch (`try`): Bubble up errors
- Try with catch (`try {} catch ($ABC$,$DEF$) {}`): Handles errors, must have at least one case
- Try with abort (`try abort {}`): Terminates program on error
- Try optional (`try?`): if error -> null

```cpp
try {
    v: f64 = sqrt(-1);
} // Callee must handle
// Try with catch
try {
    v: f64 = sqrt(-1);
} catch ($MATHERR$) {
    fmt.println("Cannot take square root of negative value.");
}
// Try with abort
try abort {
    v: f64 = sqrt(-1); // exits
}
// Try optional
v: f32? = try? sqrt(-1);
if (v) {
    fmt.printf("%\n", v);
}
```

### Example code

```cpp

struct Vector2<T> {
    x: T, y: T;

    public Vector2() {
        x = 0; // forces number at compile time
        y = 0;
    }

    public add(rhs: Vector2<T>&) {
        x += rhs.x;
        y += rhs.y;
    }
}

struct List<T> { // let language catch the problems at call site? like it would normally
    private ptr: T*? = null;
    private len: usize = 0;
    private cap: usize = 0;

    public List(initial_capacity: usize) {
        ptr = try abort alloc::<T>(initial_capacity);
        len = 0;
        cap = initial_capacity;
    }

    private ensure_capacity(offset: usize) {
        update: bool = false;
        while (len + offset > cap) {
            cap = cap * 2 + 4;
            update = true;
        }
        if (update) {
            ptr = try abort realloc::<T>(ptr, cap);
        }
    }

    public add(move v: T) {
        ensure_capacity(1);
        init(&ptr[len], move v);
        ++len;
    }

    public operator [](i: usize) -> T& fails{IDXOOB} {
        if (i >= len) {
            return $IDXOOB$;
        }
        return ptr[i];
    }

    public operator [](i: usize, move v: T) -> fails{IDXOOB} { // should v be pointer
        if (i >= len) {
            return $IDXOOB$;
        }
        ptr[i] = move v;
    }

    public operator [](start: usize, end: usize) -> T[] fails{IDXOOB} {
        if (end >= len || start >= len || start > end) {
            return $IDXOOB$;
        }
        return ptr[start...end];
    }

    public ~List() {
        if comptime (T.has_destructor) {
            for (i: usize = 0; i < len; ++i) {
                destroy(&ptr[i]);
            }
        }
        free(ptr);
    }
}

public sqrt(x: f64) -> f64 fails{MATHERR} { // if function has any return $ route it must be marked as fails or compiler errors ()
    if (x < 0) {
        return $MATHERR$; // character set is [A-Z_][A-Z0-9_]*
    }
    return ...;
}

public main() -> i32 fails{MATHERR} {
    fmt.println("Hurray!");

    fmt.printf("Hello, %\n", "World");
    my_str: string = fmt.sprintf("Hello, %\n", "World");

    try {
        sqrt(-1); // defer here too
    } // bubble up

    try abort {
        list: List<string> = List::<string>();
        list.add(string("Hello, world"));
        list.add(move my_str);
    }

    // free(my_str); no longer valid

    try {
        sqrt(4);
    } catch ($MATHERR$) {
         // if this catch wasn't it, passes through and funtion must be marked fails
         fmt.println("Math error.");
    }

    return 0;
}
```

## Compiler Stages

- Lexer
- Parser
- Barrier
- Symbol extraction and generic extraction internal representation
- Type checking
- IR codegen
- Exporting symbols, version, name, hash, generics into .hmeta inside object files using memory dump
- Import back with mmap
