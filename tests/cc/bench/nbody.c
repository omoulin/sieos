/* nbody.c - benchmark: floating point (five bodies, 4M steps of a simple integrator).
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
int printf(const char *fmt, ...);
double sqrt(double x);
typedef struct { double x, y, z, vx, vy, vz, m; } Body;
static Body b[5] = {
    { 0, 0, 0, 0, 0, 0, 39.47 }, { 4.84, -1.16, -0.10, 0.60, 2.81, -0.02, 0.037 },
    { 8.34, 4.12, -0.40, -1.01, 1.82, 0.008, 0.011 }, { 12.89, -15.11, -0.22, 1.08, 0.86, -0.01, 0.0017 },
    { 15.37, -25.91, 0.17, 0.97, 0.59, -0.03, 0.002 } };
static void step(double dt)
{
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++) {
            double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
            double d2 = dx * dx + dy * dy + dz * dz, mag = dt / (d2 * sqrt(d2));
            b[i].vx -= dx * b[j].m * mag; b[i].vy -= dy * b[j].m * mag; b[i].vz -= dz * b[j].m * mag;
            b[j].vx += dx * b[i].m * mag; b[j].vy += dy * b[i].m * mag; b[j].vz += dz * b[i].m * mag;
        }
    for (int i = 0; i < 5; i++) { b[i].x += dt * b[i].vx; b[i].y += dt * b[i].vy; b[i].z += dt * b[i].vz; }
}
int main(void)
{
    for (int k = 0; k < 4000000; k++) step(0.001);
    printf("%.6f %.6f\n", b[1].x, b[4].vz);
    return 0;
}
