#!/bin/sh
# hold.sh <name> <generations>: slow.sh's loop with NOTHING fading (2026-11-20). slow2 showed the fade to zero ends in a
# Free Peoples that leaves the Ring and raids Orthanc + Moria; raid1.sh showed 8 minutes of plain training against a mix
# that includes that raider lifts the Shadow against all of them. So: the written guidance stays where the keepers were
# trained (formula 0.30, nudges 0.3, leans 0.1, AlphaWOTR lessons 0.5), and the raider is a permanent opponent.
# One generation = the Free Peoples train ROUNDS rounds against the Shadow pool, then Shadow trains ROUNDS rounds against
# the Free Peoples pool; pools = the anchors + the newest generations (six opponents at most). Then both are read alone,
# N games a line -> runs/<name>/progress.txt. Nothing is overwritten; the folder must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; G=$2; d=runs/$n; mkdir $d || exit 1
R=${ROUNDS:-40}; RAID=runs/slow2/fp31/latest
ASH="runs/keep/shadow_aw6 runs/keep/shadow_hunter1"; AFP="runs/keep/fp_aw4 runs/keep/fp_aw7 $RAID"
SHP=""; FPP=""; csh=${SH0:-runs/raid1/latest}; cfp=${FP0:-runs/keep/fp_aw7}
E="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1"
pool() { k=$1; a=$2; shift 2; echo $a $(echo $* | tr ' ' '\n' | tail -$k) | tr ' ' ','; }
ev() { env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_x eval $1 $2 ${N:-600} 9 2>&1 | grep '^FP=' | sed 's/.*| Shadow wins \([0-9.]*\)% | FP ring \([0-9.]*\)% FP military \([0-9.]*\)% | SH corruption \([0-9.]*\)% SH military \([0-9.]*\)%.*turns \([0-9.]*\) fsp.*/Shadow \1 (conquest \5, corruption \4; Ring \2, Free Peoples conquest \3; \6 turns)/'; }
g=1
while [ $g -le $G ]; do
  f=$d/fp$g; mkdir $f; for x in fp sh pfp psh; do cp $cfp.$x $f/latest.$x; done; echo 4200 > $f/rounds
  env $E WOTR_FREEZE_SH=$(pool 4 "$ASH" $SHP) WOTR_AWLES_FP=0.5,$(cat runs/keep/aw_fp_files.txt) ./bin/wotr_x train $f $R 2000 9 960 > $f/train.log 2>&1
  cfp=$f/latest; FPP="$FPP $cfp"
  s=$d/sh$g; mkdir $s; for x in fp sh pfp psh; do cp $csh.$x $s/latest.$x; done; echo 4200 > $s/rounds
  env $E WOTR_FREEZE_FP=$(pool 3 "$AFP" $FPP) WOTR_AWLES_SH=0.5,$(cat runs/keep/aw_shadow_files.txt) ./bin/wotr_x train $s $R 2000 9 960 > $s/train.log 2>&1
  csh=$s/latest; SHP="$SHP $csh"
  {
    echo "generation $g $(date +%H:%M)"
    echo "  new Shadow vs fp_aw4:           $(ev runs/keep/fp_aw4 $csh)"
    echo "  new Shadow vs fp_aw7:           $(ev runs/keep/fp_aw7 $csh)"
    echo "  new Shadow vs the raider:       $(ev $RAID $csh)"
    echo "  shadow_aw6 vs new Free Peoples: $(ev $cfp runs/keep/shadow_aw6)"
    echo "  hunter vs new Free Peoples:     $(ev $cfp runs/keep/shadow_hunter1)"
    echo "  new Shadow vs new Free Peoples: $(ev $cfp $csh)"
  } >> $d/progress.txt
  g=$((g + 1))
done
touch $d/done
