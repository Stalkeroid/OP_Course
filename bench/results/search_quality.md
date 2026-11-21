| # | query | expected | top-3 actual (doc_id, score) | result |
|---:|---|---|---|---|
| 1 | `building` | architecture/* | architecture/doc_006240.txt (1.0); architecture/doc_008944.txt (0.999871); architecture/doc_006128.txt (0.998771) | PASS |
| 2 | `recipe` | cooking/* | cooking/doc_010547.txt (1.0); cooking/doc_008739.txt (0.998695); cooking/doc_010899.txt (0.998564) | PASS |
| 3 | `savings` | finance/* | finance/doc_019094.txt (1.0); finance/doc_007462.txt (0.998921); finance/doc_000630.txt (0.995356) | PASS |
| 4 | `molecule` | science/* | science/doc_016873.txt (1.0); science/doc_011913.txt (0.998927); science/doc_010265.txt (0.997999) | PASS |
| 5 | `software hardware` | technology/* | technology/doc_017852.txt (1.0); technology/doc_012716.txt (0.998692); technology/doc_007916.txt (0.998483) | PASS |
| 6 | `hail thunder` | weather/* | weather/doc_009119.txt (1.0); weather/doc_012575.txt (0.99778); weather/doc_014831.txt (0.9964) | PASS |
| 7 | `enterprise manager` | business/* | business/doc_000258.txt (1.0); business/doc_014562.txt (0.998858); business/doc_001634.txt (0.998632) | PASS |
| 8 | `desert meadow wetland` | environment/* | environment/doc_017285.txt (1.0); environment/doc_015301.txt (0.999877); environment/doc_001573.txt (0.998857) | PASS |
| 9 | `chord tune tempo` | music/* | music/doc_002616.txt (1.0); music/doc_006200.txt (0.999997); music/doc_010712.txt (0.99881) | PASS |
| 10 | `zxqvwk` | no results | (none) | PASS |

Passed 10 of 10. Expected topic = folder of the vocab file that holds the word(s).
