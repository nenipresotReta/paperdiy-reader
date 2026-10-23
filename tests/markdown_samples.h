#pragma once
namespace markdown_samples {
static const char mixed[] = R"MD(# 数学笔记

这是 **Markdown** 与 *mathematics* 的混合排版。行内公式 $E=mc^2$ 跟随正文的共同基线。

## 分式与根号

$$
x=\frac{-b\pm\sqrt{b^2-4ac}}{2a}
$$

正文应接在公式下方，不能被分母或上下标遮挡。An inline fraction $\frac{a_1+b_2}{c^2}$ also changes the line height.

> 引用中的数学：$\alpha+\beta=\gamma$。

1. 保留每本笔记的原文位置。
2. 未支持的公式保留完整源码：$\unsupported{x}$。

## 求和与矩阵

$$
\sum_{k=1}^{n} k = \frac{n(n+1)}{2}
$$

$$
A=\begin{pmatrix}a & b \\ c & d\end{pmatrix}
$$

```text
literal $not_math$
value = (a + b) / 2
```

末尾正文依然可读。The final paragraph stays visible.
)MD";
}
