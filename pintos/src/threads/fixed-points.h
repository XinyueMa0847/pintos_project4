/* simulate float arithmetics using integers 'x' and 'y' are assumed
   to be fixed point numbers while n is an integer*/


//Fraction part: 14bits are used, thus f=2^14;
#define F (1<<14) 

//converting integer to fixed point
int
n_to_f(int n){return n*F;}

//converting fixed point to integer, round towards zero
int
f_to_n_zero(int x){return x/F;}

//conterting fixed point to integer, round towards nearest integer
int
f_to_n_nearest(int x){return x>=0 ? (x+F/2)/F : (x-F/2)/F;}

//add two fixed point numbers x and y
int
add_x_y(int x, int y){return x+y;}

//subtract y from x
int
sub_x_y(int x,int y){return x-y;}

//add x and n
int
add_x_n(int x, int n){return x+n*F;}

//subtract n from x
int
sub_x_n(int x, int n){ return x-n*F;}

//multiply x by y
int
mul_x_y(int x, int y){ return ((int64_t)x)*y/F;}

//multiply x by n
int 
mul_x_n(int x,int n){return x*n;}

//Divide x by y
int
div_x_y(int x,int y){return ((int64_t)x)*F/y;}

//divede x by n
int
div_x_n(int x, int n){return x/n;}
