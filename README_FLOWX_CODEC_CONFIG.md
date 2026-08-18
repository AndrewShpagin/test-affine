# FlowX codec product configuration

Current product-level mesh controls:

```json
"codec": {
  "mesh": true,
  "mesh_grid_x": 6,
  "mesh_grid_y": 6
}
```

The AFC1 patch wire format already carries `grid_x` and `grid_y`, so changing the transmitted mesh grid does not require a protocol-version change. The browser decoder currently supports grids up to 8x8.

The core motion estimator still produces its established 6x6 residual field; the sender reshapes that field to the configured transmitted grid. This keeps the current estimator stable while exposing the bandwidth/decoder-quality control at the product boundary. Setting `mesh=false` transmits a 1x1 zero residual field, so receivers apply no mesh deformation.
