; 
(set-info :status unknown)
(declare-fun standard_metadata.ingress_port () (_ BitVec 9))
(declare-fun standard_metadata.egress_spec () (_ BitVec 9))
(declare-fun ethernet.ether_type () (_ BitVec 16))
(declare-fun ethernet.src_addr () (_ BitVec 48))
(assert
 (let (($x90 (= standard_metadata.ingress_port (_ bv7 9))))
 (let (($x85 (= standard_metadata.ingress_port (_ bv6 9))))
 (let (($x80 (= standard_metadata.ingress_port (_ bv5 9))))
 (let (($x75 (= standard_metadata.ingress_port (_ bv4 9))))
 (let (($x70 (= standard_metadata.ingress_port (_ bv3 9))))
 (let (($x66 (= standard_metadata.ingress_port (_ bv2 9))))
 (let (($x62 (= standard_metadata.ingress_port (_ bv1 9))))
 (let (($x67 (or (or (or false (= standard_metadata.ingress_port (_ bv0 9))) $x62) $x66)))
 (or (or (or (or (or $x67 $x70) $x75) $x80) $x85) $x90))))))))))
(assert
 (let ((?x36 (ite (and true (not (and true (= ethernet.ether_type (_ bv16 16))))) (_ bv511 9) standard_metadata.egress_spec)))
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let (($x44 (and true (and (distinct (ite $x31 0 (- 1)) (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x54 (ite $x48 (_ bv3 9) (ite $x31 (_ bv2 9) ?x36))))
 (let (($x73 (or (or (or (or false (= ?x54 (_ bv0 9))) (= ?x54 (_ bv1 9))) (= ?x54 (_ bv2 9))) (= ?x54 (_ bv3 9)))))
 (let (($x93 (or (or (or (or $x73 (= ?x54 (_ bv4 9))) (= ?x54 (_ bv5 9))) (= ?x54 (_ bv6 9))) (= ?x54 (_ bv7 9)))))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (or $x49 $x93))))))))))))
(assert
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let ((?x38 (ite $x31 0 (- 1))))
 (let ((?x41 (ite $x31 (_ bv2 9) (ite (and true (not $x30)) (_ bv511 9) standard_metadata.egress_spec))))
 (let (($x44 (and true (and (distinct ?x38 (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x54 (ite $x48 (_ bv3 9) ?x41)))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (and (and (not $x49) true) (= ?x38 (- 1)))))))))))))
(check-sat)

; 
(set-info :status unknown)
(declare-fun standard_metadata.ingress_port () (_ BitVec 9))
(declare-fun standard_metadata.egress_spec () (_ BitVec 9))
(declare-fun ethernet.ether_type () (_ BitVec 16))
(declare-fun ethernet.src_addr () (_ BitVec 48))
(assert
 (let (($x90 (= standard_metadata.ingress_port (_ bv7 9))))
 (let (($x85 (= standard_metadata.ingress_port (_ bv6 9))))
 (let (($x80 (= standard_metadata.ingress_port (_ bv5 9))))
 (let (($x75 (= standard_metadata.ingress_port (_ bv4 9))))
 (let (($x70 (= standard_metadata.ingress_port (_ bv3 9))))
 (let (($x66 (= standard_metadata.ingress_port (_ bv2 9))))
 (let (($x62 (= standard_metadata.ingress_port (_ bv1 9))))
 (let (($x67 (or (or (or false (= standard_metadata.ingress_port (_ bv0 9))) $x62) $x66)))
 (or (or (or (or (or $x67 $x70) $x75) $x80) $x85) $x90))))))))))
(assert
 (let ((?x36 (ite (and true (not (and true (= ethernet.ether_type (_ bv16 16))))) (_ bv511 9) standard_metadata.egress_spec)))
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let (($x44 (and true (and (distinct (ite $x31 0 (- 1)) (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x54 (ite $x48 (_ bv3 9) (ite $x31 (_ bv2 9) ?x36))))
 (let (($x73 (or (or (or (or false (= ?x54 (_ bv0 9))) (= ?x54 (_ bv1 9))) (= ?x54 (_ bv2 9))) (= ?x54 (_ bv3 9)))))
 (let (($x93 (or (or (or (or $x73 (= ?x54 (_ bv4 9))) (= ?x54 (_ bv5 9))) (= ?x54 (_ bv6 9))) (= ?x54 (_ bv7 9)))))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (or $x49 $x93))))))))))))
(assert
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let ((?x38 (ite $x31 0 (- 1))))
 (let ((?x41 (ite $x31 (_ bv2 9) (ite (and true (not $x30)) (_ bv511 9) standard_metadata.egress_spec))))
 (let (($x44 (and true (and (distinct ?x38 (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x54 (ite $x48 (_ bv3 9) ?x41)))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (let (($x207 (and (not $x49) true)))
 (and $x207 (= ?x38 0)))))))))))))
(check-sat)

; 
(set-info :status unknown)
(declare-fun standard_metadata.ingress_port () (_ BitVec 9))
(declare-fun standard_metadata.egress_spec () (_ BitVec 9))
(declare-fun ethernet.ether_type () (_ BitVec 16))
(declare-fun ethernet.src_addr () (_ BitVec 48))
(assert
 (let (($x90 (= standard_metadata.ingress_port (_ bv7 9))))
 (let (($x85 (= standard_metadata.ingress_port (_ bv6 9))))
 (let (($x80 (= standard_metadata.ingress_port (_ bv5 9))))
 (let (($x75 (= standard_metadata.ingress_port (_ bv4 9))))
 (let (($x70 (= standard_metadata.ingress_port (_ bv3 9))))
 (let (($x66 (= standard_metadata.ingress_port (_ bv2 9))))
 (let (($x62 (= standard_metadata.ingress_port (_ bv1 9))))
 (let (($x67 (or (or (or false (= standard_metadata.ingress_port (_ bv0 9))) $x62) $x66)))
 (or (or (or (or (or $x67 $x70) $x75) $x80) $x85) $x90))))))))))
(assert
 (let ((?x36 (ite (and true (not (and true (= ethernet.ether_type (_ bv16 16))))) (_ bv511 9) standard_metadata.egress_spec)))
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let (($x44 (and true (and (distinct (ite $x31 0 (- 1)) (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x54 (ite $x48 (_ bv3 9) (ite $x31 (_ bv2 9) ?x36))))
 (let (($x73 (or (or (or (or false (= ?x54 (_ bv0 9))) (= ?x54 (_ bv1 9))) (= ?x54 (_ bv2 9))) (= ?x54 (_ bv3 9)))))
 (let (($x93 (or (or (or (or $x73 (= ?x54 (_ bv4 9))) (= ?x54 (_ bv5 9))) (= ?x54 (_ bv6 9))) (= ?x54 (_ bv7 9)))))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (or $x49 $x93))))))))))))
(assert
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let ((?x38 (ite $x31 0 (- 1))))
 (let (($x44 (and true (and (distinct ?x38 (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x51 (ite $x48 0 (- 1))))
 (let ((?x41 (ite $x31 (_ bv2 9) (ite (and true (not $x30)) (_ bv511 9) standard_metadata.egress_spec))))
 (let ((?x54 (ite $x48 (_ bv3 9) ?x41)))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (and (and (not $x49) $x44) (= ?x51 (- 1))))))))))))))
(check-sat)

; 
(set-info :status unknown)
(declare-fun standard_metadata.ingress_port () (_ BitVec 9))
(declare-fun standard_metadata.egress_spec () (_ BitVec 9))
(declare-fun ethernet.ether_type () (_ BitVec 16))
(declare-fun ethernet.src_addr () (_ BitVec 48))
(assert
 (let (($x90 (= standard_metadata.ingress_port (_ bv7 9))))
 (let (($x85 (= standard_metadata.ingress_port (_ bv6 9))))
 (let (($x80 (= standard_metadata.ingress_port (_ bv5 9))))
 (let (($x75 (= standard_metadata.ingress_port (_ bv4 9))))
 (let (($x70 (= standard_metadata.ingress_port (_ bv3 9))))
 (let (($x66 (= standard_metadata.ingress_port (_ bv2 9))))
 (let (($x62 (= standard_metadata.ingress_port (_ bv1 9))))
 (let (($x67 (or (or (or false (= standard_metadata.ingress_port (_ bv0 9))) $x62) $x66)))
 (or (or (or (or (or $x67 $x70) $x75) $x80) $x85) $x90))))))))))
(assert
 (let ((?x36 (ite (and true (not (and true (= ethernet.ether_type (_ bv16 16))))) (_ bv511 9) standard_metadata.egress_spec)))
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let (($x44 (and true (and (distinct (ite $x31 0 (- 1)) (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x54 (ite $x48 (_ bv3 9) (ite $x31 (_ bv2 9) ?x36))))
 (let (($x73 (or (or (or (or false (= ?x54 (_ bv0 9))) (= ?x54 (_ bv1 9))) (= ?x54 (_ bv2 9))) (= ?x54 (_ bv3 9)))))
 (let (($x93 (or (or (or (or $x73 (= ?x54 (_ bv4 9))) (= ?x54 (_ bv5 9))) (= ?x54 (_ bv6 9))) (= ?x54 (_ bv7 9)))))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (or $x49 $x93))))))))))))
(assert
 (let (($x29 (= ethernet.ether_type (_ bv16 16))))
 (let (($x30 (and true $x29)))
 (let (($x31 (and true $x30)))
 (let ((?x38 (ite $x31 0 (- 1))))
 (let (($x44 (and true (and (distinct ?x38 (- 1)) true))))
 (let (($x48 (and $x44 (and true (= ethernet.src_addr (_ bv256 48))))))
 (let ((?x51 (ite $x48 0 (- 1))))
 (let ((?x41 (ite $x31 (_ bv2 9) (ite (and true (not $x30)) (_ bv511 9) standard_metadata.egress_spec))))
 (let ((?x54 (ite $x48 (_ bv3 9) ?x41)))
 (let (($x49 (= ?x54 (_ bv511 9))))
 (and (and (not $x49) $x44) (= ?x51 0)))))))))))))
(check-sat)
