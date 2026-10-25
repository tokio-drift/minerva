// Feature: arrays (integer and char)
int sum(int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) s += a[i];
  return s;
}

int length(char s[]) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

void reverse(char *s) {
  int n = length(s);
  for (int i = 0; i < n / 2; i++) {
    char t = s[i];
    s[i] = s[n - 1 - i];
    s[n - 1 - i] = t;
  }
}

int main() {
  // integer arrays: full, partial (rest is zero) and element updates
  int a[5] = {1, 2, 3, 4, 5};
  int b[6] = {7, 8};
  int c[100] = {9};
  a[2] = a[0] + a[4];
  a[1] *= 10;
  for (int i = 0; i < 5; i++) printf("%d ", a[i]);
  printf("\n%d %d %d %d\n", b[1], b[2], b[5], c[99]);
  printf("sum = %d\n", sum(a, 5));

  // computed indices and counting
  int data[10] = {1, 3, 2, 3, 4, 1, 1, 0, 4, 4};
  int counts[5] = {0};
  for (int i = 0; i < 10; i++) 
  for (int i = 0; i < 5; i++) printf("%d ", counts[i]);
  printf("\n");

  // char arrays: initialised from a string, element access, modification
  // char s[] = "hello";
  // char t[10] = "ab";
  // printf("%s %s %d %d\n", s, t, length(s), length(t));
  // reverse(s);
  // s[0] = 'O';
  // printf("%s\n", s);
  // char letters[4] = {'w', 'x', 'y', 'z'};
  // for (int i = 0; i < 4; i++) printf("%c", letters[i]);
  // printf("\n");

  // build a string by hand
  char buf[8];
  char ch = 'a';
  for (int i = 0; i < 5; i++) { buf[i] = ch; ch++; }
  buf[5] = 0;
  printf("%s %d\n", buf, length(buf));

  // double and unsigned char arrays
  // double d[3] = {0.5, 1.5, 2.5};
  // unsigned char bytes[3] = {10, 200, 255};
  // printf("%f %d\n", d[0] + d[2], bytes[1] + bytes[2]);
  return 0;
}
