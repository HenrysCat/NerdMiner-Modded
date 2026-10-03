# The production schedule (pipelined_hw_mine_classic_v2), the fallback, each
# delay at the lowest value that survives worst-case core-0 bus traffic, and
# all of them one cycle lower -- which must fail under stress, or the bench
# has stopped being able to see a missed threshold.
base(p1=6, d2=16, p2=42, pl1=12, d3=14, p3=29, pl2=12, lpos=2, wgap=1, iram=1)
add("prod")
add("prod_nochk", None, chk=0)
add("wide", None, p1=18, p2=54, pl1=24, p3=41, pl2=24)
add("thr", "p1", p1=4)
add("thr", "p2", p2=40)
add("thr", "p3", p3=27)
add("thr", "pl1", pl1=10)
add("thr", "pl2", pl2=10)
add("thr", "all", p1=4, p2=40, p3=27, pl1=10, pl2=10)
add("below", "all", p1=3, p2=39, p3=26, pl1=9, pl2=9)
# Same timing with TEXT stores only two cycles apart: must fail (rule 1).
add("wgap0", None, wgap=0, p1=22, p3=37, pl2=20)
