# Introduction

This code helps you write to the Linux kernel's ftrace buffer (see
https://www.kernel.org/doc/Documentation/trace/ftrace.txt for some additional
information) from userspace. Android's systrace tool is able to read the
output generated by this code create pretty interactive diagrams of it.

There are three types of events you can write with this code.

Firstly, duration events. A duration event is an event that has a strict
requirement of being nested (such as in a call stack, for example). To trace
duration events, use systrace_duration_begin() and systrace_duration_end()
pairs. Alternatively, if you are using C++, you may use the CSystraceEvent
wrapper class provided.

    void Foo::myFoo()
    {
        systrace_duration_begin("app", "Foo::myFoo_C");
        CSystraceEvent ev("app", "Foo::myFoo");
        systrace_duration_end("app", "Foo::myFoo_C");
    }

Secondly, asynchronous events. An asynchronous event does not necessarily
have to be nested. It is something tied to e.g. an ongoing network transfer.
To write asynchronous events, use systrace_async_begin() and
systrace_async_end(). Alternatively, if you are using C++, you may use the
CSystraceAsyncEvent wrapper class provided.

    void Foo::myFoo()
    {
        void *p;
        systrace_async_begin("app", "Foo::myFoo_C", p);
        CSystraceAsyncEvent ev("app", "Foo::myFoo", p);
        systrace_async_end("app", "Foo::myFoo_C", p);
    }

Finally, counter events. A counter event is simply a way of showing the
progression of a value over time. For instance, one may wish to count the
number of available free buffers when rendering graphics. To write a counter,
use systrace_record_counter().

    void Foo::myFoo()
    {
        systrace_record_counter("app", "freeFoo", 10);
    }

This code talks about two different terms: a module, and a tracepoint.
A module is a hidden implementation detail that lets you organise your
tracepoints, and subsequently filter them (in systrace_should_trace()), so you
can only focus on tracepoints that interest you. A tracepoint is a
human-readable string, such as a function name for duration and asynchronous
events, or a variable name in the case of counters.

Note! Writing events is not free. Realise that the more tracepoints you add,
the slower your code will get. Make sure to remove tracepoints that you do
not need (or disable them by changing systrace_should_trace()'s
implementation).

Note! This code has absolutely no API or ABI stability guarentees. It also
has no stability guarentees in general, but it probably won't eat your pet dog.

# Useful handy hints

* `adb install` will get an apk on the device
* `adb shell am force-stop org.myfoo.mybar` will stop the process
* `adb shell am start -n org.myfoo.mybar/myActivity.Thing.Here` will start it
* `systrace.py --time=20 -o mynewtrace.html sched gfx view wm app --timeout=120
--collection-timeout=120 --buf-size=102400` will help you trace it (add more
buffer as needed, depending on the amount of tracing you do, and the number of
seconds you collect for).