#include "LuaMemTypeDoc.h"

namespace LuaMemPreview
{
const char* TypeSummary(const std::string& t)
{
    if (t == "table")        return "Lua 表：表头 + 数组部分 + 哈希部分";
    if (t == "string")       return "字符串内容本身（短串 + 长串）";
    if (t == "userdata")     return "userdata 数据块 + uservalue 槽";
    if (t == "lua_closure")  return "Lua 闭包：闭包头 + upvalue 指针数组";
    if (t == "c_closure")    return "C 闭包：闭包头 + upvalue 值数组";
    if (t == "proto")        return "函数原型：字节码 + 常量表 + 调试信息";
    if (t == "upval")        return "被闭包捕获、已离开栈的 upvalue 盒子";
    if (t == "thread")       return "协程：线程块 + 值栈 + CallInfo 链（含 global_State）";
    if (t == "string_table") return "字符串驻留哈希表的桶数组（不含串内容）";
    if (t == "vm_other")     return "残差对账：Lua 自报总量减去以上各项";
    return "未知分类标记（新增桶？本工具的类型表需同步）";
}

const char* TypeDetail(const std::string& t)
{
    if (t == "table")
        return "每个 table 的完整占用：Table 结构体本体，加上它拥有的数组部分\n"
               "和哈希部分（node 数组）。\n\n"
               "表增长时按『新总量』重新上报，不是累加增量，所以反复 rehash\n"
               "不会把同一个表重复记账。";

    if (t == "string")
        return "字符串对象本体 + 字符数据。短串（<=40 字节）会驻留，相同内容\n"
               "只有一份；长串每次独立分配。\n\n"
               "注意它不含驻留哈希表的桶数组，那部分单独计在 string_table。\n"
               "如果 string 不大而 string_table 很大，说明不同内容的短串数量\n"
               "很多，而不是字符串本身占用大。";

    if (t == "userdata")
        return "完整 userdata 的数据块，加上 uservalue 槽位。\n\n"
               "它只统计 Lua 侧这一块内存。userdata 背后常常持有 C/C++ 对象，\n"
               "那部分不在 Lua 堆里，这里看不到，也不该指望用它衡量宿主侧占用。";

    if (t == "lua_closure")
        return "LClosure 结构体 + upvalue 指针数组。\n\n"
               "不含函数原型——字节码、常量、调试信息都在 proto，且多个闭包\n"
               "共享同一个 proto。所以同一函数创建 N 个闭包，proto 只有一份。";

    if (t == "c_closure")
        return "CClosure 结构体 + upvalue 值数组（直接内联存值，不是指针）。\n\n"
               "C 函数本身是宿主的代码段，不占 Lua 堆。";

    if (t == "proto")
        return "函数原型的完整足迹：Proto 头，加上它拥有的全部动态数组——\n"
               "字节码（code）、常量表（k）、嵌套原型（p）、行号表（lineinfo）、\n"
               "局部变量名（locvars）、upvalue 描述（upvalues）。\n\n"
               "这些动态数组是后来才纳入统计的。在那之前 proto 只上报结构体\n"
               "头部，整体不足 1 MB；纳入后它成了参考快照里最大的一项\n"
               "（45.24 MB / 44.94%）。也就是说这部分内存一直存在，只是以前\n"
               "统计看不到它。\n\n"
               "它随『加载了多少代码』增长，与运行时数据量无关，靠 GC 压不下来；\n"
               "要降只能少加载代码，或剥掉调试信息。";

    if (t == "upval")
        return "UpVal 盒子。闭包捕获的变量离开栈之后，值被搬进这里独立存放。\n\n"
               "仍在栈上的 upvalue 不单独计入——那段内存属于所在 thread 的\n"
               "值栈。";

    if (t == "thread")
        return "每个协程的完整占用：lua_State 块 + 值栈 + CallInfo 链。\n\n"
               "global_State（VM 全局状态）与主线程同处一个 malloc 块\n"
               "（struct LG），因此计入本项，而不是 vm_other。要查 VM 全局\n"
               "开销就看这里。\n\n"
               "值栈只会增长、不会自动收缩到峰值以下，所以一个深递归过后的\n"
               "协程会持续占着高位栈空间。";

    if (t == "string_table")
        return "全局字符串驻留表的桶数组（指针数组）本身，不是 GC 对象。\n\n"
               "只随『不同内容的短串有多少种』增长，与串的长度无关。\n"
               "它和 string 是两件事：string 是内容，这里是索引结构。";

    if (t == "vm_other")
        return "残差项，不是实测分类：= Lua 自报总量 − 前 9 项之和。\n\n"
               "对应不属于任何 GC 对象的 VM 内部内存，比如词法/语法分析的\n"
               "临时缓冲。\n\n"
               "它是这份统计的自检位。为 0 说明前 9 项已把所有字节认领干净；\n"
               "数值明显偏大则意味着有分配路径没被覆盖，此时各分类占比都该\n"
               "打折看待。参考快照里它是 0 B。";

    return "";
}

bool IsNonGcBucket(const std::string& t)
{
    return t == "string_table" || t == "vm_other";
}

}  // namespace LuaMemPreview
