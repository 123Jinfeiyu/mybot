@Robot
 name   max  min  offset     note   type
   s1    90    -       -    SERVO    f32 	# rb0
   s2   180    -       -        -      - 	# rb1 
   s3   -90    -       -        -      -	# lb0
   s4  -180    -       -        -      -	# lb1
   s5    90    -       -        -      -	# lf0
   s6   180    -       -        -      -	# lf1
   s7   -90    -       -        -      -	# rf0
   s8  -180    -       -        -      -	# rf1
   s9   180    -       -        -      -	# cam0
  s10   180    -       -        -      -	# cam1
  spk  5000    -       -  SPEAKER    i32	# speaker
delay  1000    -       -     TIME    f32	# delay
@

@Action
s1    s2  s3   s4  s5   s6  s7   s8   s9 s10   spk  delay;	# column name
45     -  45    -  45    -  45    -   90  90   10     200;	# stand
90     -   -    -   -    -   -    -    -   -    -      20;	# left-behind

45     -   -    -   -    -   -    -    -   -    0     500;	# stand
 -     -  90    -   -    -  45    -    -   -    -      20;	# right-behind

 -     -  45    -   -    -   -    -    -   -  200     500;	# stand
 -     -   -    -  90    -   -    -    -   -    0      20;	# right-front

 -     -   -    -  45    -   -    -    -   -    -     500;	# stand
 -     -   -    -   -    -  90    -    -   -    -      20;	# left-front

 -     -   -    -   -    -  45    -    -   -    -     500;	# stand
 -     -   -    -   -    -   -    -    -   -    -      20;	# stand
@
