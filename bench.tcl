proc bench {title script} {
    while {[string length $title] < 20} {
	append title " "
    }
    puts "$title - [time $script]"
}

### BUSY LOOP ##################################################################

proc x {} {
    set i 0
    while {$i < 1850000} {
	incr i
    }
}

### FIBONACCI ##################################################################

proc fibonacci {x} {
    if {$x <= 1} {
	expr 1
    } else {
	expr {[fibonacci [expr {$x-1}]] + [fibonacci [expr {$x-2}]]}
    }
}

### HEAPSORT ###################################################################

set IM 139968
set IA   3877
set IC  29573

set last 42

proc make_gen_random {} {
    global IM IA IC
    set body "
        global last
        expr {(\$max * \[set last \[expr {(\$last * $IA + $IC) % $IM}\]\]) / $IM}
    "
    proc gen_random {max} $body
}

proc heapsort {ra_name} {
    upvar 1 $ra_name ra
    set n [llength $ra]
    set l [expr {$n / 2}]
    set ir [expr {$n - 1}]
    while 1 {
        if {$l} {
            set rra [lindex $ra [incr l -1]]
        } else {
	    set rra [lindex $ra $ir]
	    lset ra $ir [lindex $ra 0]
	    if {[incr ir -1] == 0} {
                lset ra 0 $rra
		break
            }
        }
	set i $l
	set j [expr {(2 * $l) + 1}]
        while {$j <= $ir} {
	    set tmp [lindex $ra $j]
	    if {$j < $ir} {
		if {$tmp < [lindex $ra [expr {$j + 1}]]} {
		    set tmp [lindex $ra [incr j]]
		    #eval [list a b c]
		}
	    }
            if {$rra >= $tmp} {
		break
	    }
	    lset ra $i $tmp
	    incr j [set i $j]
        }
        lset ra $i $rra
    }
}

proc heapsort_main {} {
    set n 6100
    make_gen_random

    set data {}
    for {set i 1} {$i <= $n} {incr i} {
	lappend data [gen_random 1.0]
    }
    heapsort data
}

### SIEVE ######################################################################

proc sieve {num} {
    while {$num > 0} {
	incr num -1
	set count 0
	for {set i 2} {$i <= 8192} {incr i 1} {
	    set flags($i) 1
	}
	for {set i 2} {$i <= 8192} {incr i 1} {
	    if {$flags($i) == 1} {
		# remove all multiples of prime: i
		for {set k [expr {$i+$i}]} {$k <= 8192} {incr k $i} {
		    set flags($k) 0
		}
		incr count 1
	    }
	}
    }
    return $count
}

### ARY ########################################################################

proc ary n {
    for {set i 0} {$i < $n} {incr i} {
	set x($i) $i
    }
    set last [expr {$n - 1}]
    for {set j $last} {$j >= 0} {incr j -1} {
	set y($j) $x($j)
    }
}

### REPEAT #####################################################################

proc repeat {n body} {
    for {set i 0} {$i < $n} {incr i} {
	uplevel 1 $body
    }
}

proc use_repeat {} {
    set x 0
    repeat {1000000} {incr x}
}

### UPVAR ######################################################################

proc myincr varname {
    upvar 1 $varname x
    incr x
}

proc upvartest {} {
    set y 0
    for {set x 0} {$x < 100000} {myincr x} {
	myincr y
    }
}

### NESTED LOOPS ###############################################################

proc nestedloops {} {
    set n 10
    set x 0
    incr n 1
    set a $n
    while {[incr a -1]} {
	set b $n
	while {[incr b -1]} {
	    set c $n
	    while {[incr c -1]} {
		set d $n
		while {[incr d -1]} {
		    set e $n
		    while {[incr e -1]} {
			set f $n
			while {[incr f -1]} {
			    incr x
			}
		    }
		}
	    }
	}
    }
}

### RUN ALL ####################################################################

bench {busy loop} {x}
bench {fibonacci(25)} {fibonacci 25}
bench {heapsort} {heapsort_main}
bench {sieve} {sieve 10}
bench {ary} {ary 100000}
bench {repeat} {use_repeat}
bench {upvar} {upvartest}
bench {nested loops} {nestedloops}